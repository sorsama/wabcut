#include <wabcut/ai/translate/Segmentation.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace wabcut::ai::translate {

namespace {

double seconds(Ticks t) { return static_cast<double>(t.value) / static_cast<double>(kTicksPerSecond); }
Ticks ticksOf(double s) { return Ticks{static_cast<std::int64_t>(std::llround(s * static_cast<double>(kTicksPerSecond)))}; }

struct Work {
    Ticks start{};
    Ticks end{};
    std::string text; ///< flat
    SpeakerId speaker;
    std::vector<std::size_t> sources;
    bool extended = false;
    bool merged = false;
    bool rebalanced = false;
};

/// Languages written without spaces between words but with spaces between phrases (Thai, Lao, Khmer, Burmese):
/// two cues join with a space. Chinese and Japanese join with nothing.
bool phraseSpaced(std::string_view language) {
    const std::string l = normalizeLanguage(language);
    return l == "th" || l == "lo" || l == "km" || l == "my";
}

std::string joinText(std::string_view a, std::string_view b, std::string_view language) {
    if (a.empty()) {
        return std::string(b);
    }
    if (b.empty()) {
        return std::string(a);
    }
    const bool space = !isUnspacedLanguage(language) || phraseSpaced(language);
    return std::string(a) + (space ? " " : "") + std::string(b);
}

std::string trimmed(std::string_view s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\n' || s[b] == '\t')) {
        ++b;
    }
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\n' || s[e - 1] == '\t')) {
        --e;
    }
    return std::string(s.substr(b, e - b));
}

double cpsOf(std::string_view text, Ticks duration) {
    const double d = seconds(duration);
    return d > 0.0 ? static_cast<double>(readingLength(text)) / d : std::numeric_limits<double>::infinity();
}

bool fits(std::string_view text, std::string_view language, const LanguageRules& rules, const LineBreaker& breaker) {
    return breakCaptionLines(text, language, rules.maxCharsPerLine, rules.maxLines, breaker).has_value();
}

/// Byte offsets where a new grapheme-ish cluster starts (a code point that is not a combining mark).
std::vector<std::size_t> clusterStarts(std::string_view text) {
    std::vector<std::size_t> out;
    for (std::size_t i = 1; i < text.size(); ++i) {
        const auto byte = static_cast<unsigned char>(text[i]);
        if ((byte & 0xC0) == 0x80) {
            continue; // continuation byte
        }
        std::size_t next = i + 1;
        while (next < text.size() && (static_cast<unsigned char>(text[next]) & 0xC0) == 0x80) {
            ++next;
        }
        if (captionDisplayLength(text.substr(i, next - i)) == 0) {
            continue; // a combining mark stays with its base
        }
        out.push_back(i);
    }
    return out;
}

bool afterPunctuation(std::string_view text, std::size_t at) {
    if (at == 0) {
        return false;
    }
    std::size_t p = at;
    while (p > 0 && text[p - 1] == ' ') {
        --p;
    }
    if (p == 0) {
        return false;
    }
    const char c = text[p - 1];
    if (c == '.' || c == ',' || c == '!' || c == '?' || c == ';' || c == ':') {
        return true;
    }
    // "。" "、" "，" "！" "？" end in these bytes (U+3002 E3 80 82, U+3001 E3 80 81, U+FF0C EF BC 8C, U+FF01 EF BC 81,
    // U+FF1F EF BC 9F).
    if (p >= 3) {
        const std::string_view tail = text.substr(p - 3, 3);
        return tail == "\xE3\x80\x82" || tail == "\xE3\x80\x81" || tail == "\xEF\xBC\x8C" || tail == "\xEF\xBC\x81" ||
               tail == "\xEF\xBC\x9F";
    }
    return false;
}

/// Where `text` may be cut into cues: the breaker's opportunities, else cluster starts (flagged as overflow).
std::vector<std::size_t> cutPoints(std::string_view text, std::string_view language, const LineBreaker& breaker, bool* forced) {
    std::vector<std::size_t> points = breaker.breakOpportunities(text, language);
    std::erase_if(points, [&](std::size_t p) { return p == 0 || p >= text.size(); });
    if (points.empty()) {
        if (forced) {
            *forced = true;
        }
        points = clusterStarts(text);
    }
    return points;
}

/// The cut nearest to `target` (a display position), preferring one after punctuation within a quarter of `span`.
std::optional<std::size_t> nearestCut(std::string_view text, std::span<const std::size_t> points, double target, double span,
                                      std::size_t after) {
    std::optional<std::size_t> best;
    double bestScore = std::numeric_limits<double>::infinity();
    for (const std::size_t p : points) {
        if (p <= after) {
            continue;
        }
        const double position = static_cast<double>(captionDisplayLength(text.substr(0, p)));
        double score = std::abs(position - target);
        if (afterPunctuation(text, p) && score <= span / 4.0) {
            score *= 0.25;
        }
        if (score < bestScore) {
            bestScore = score;
            best = p;
        }
    }
    return best;
}

/// Splits a flat text into `parts` pieces at cut points near equal display lengths.
std::vector<std::string> splitText(std::string_view text, std::size_t parts, std::string_view language, const LineBreaker& breaker,
                                   bool* forced) {
    const std::vector<std::size_t> points = cutPoints(text, language, breaker, forced);
    const double total = static_cast<double>(captionDisplayLength(text));
    std::vector<std::string> out;
    std::size_t from = 0;
    for (std::size_t k = 1; k < parts; ++k) {
        const auto cut = nearestCut(text, points, total * static_cast<double>(k) / static_cast<double>(parts),
                                    total / static_cast<double>(parts), from);
        if (!cut) {
            break;
        }
        std::string piece = trimmed(text.substr(from, *cut - from));
        if (!piece.empty()) {
            out.push_back(std::move(piece));
        }
        from = *cut;
    }
    std::string last = trimmed(text.substr(from));
    if (!last.empty()) {
        out.push_back(std::move(last));
    }
    return out;
}

} // namespace

std::size_t readingLength(std::string_view text) {
    std::size_t length = captionDisplayLength(text);
    for (const char c : text) {
        if (c == '\n') {
            --length; // a line break is not read
        }
    }
    return length;
}

std::vector<TimedCue> resegment(std::span<const TranslatedCue> cues, std::string_view language, const LanguageRules& rules,
                                const TimingRules& timing, const LineBreaker& breaker, SegmentationReport* report) {
    SegmentationReport stats;
    const double limit = rules.charsPerSecond > 0.0 ? rules.charsPerSecond : 17.0;
    const auto need = [&](std::string_view text) { return ticksOf(static_cast<double>(readingLength(text)) / limit); };

    std::vector<Work> work;
    work.reserve(cues.size());
    for (const TranslatedCue& c : cues) {
        Work w;
        w.start = c.start;
        w.end = c.start + std::max(c.duration, Ticks{1});
        w.text = flattenCaptionText(c.text, language);
        w.speaker = c.speaker;
        w.sources = {c.source};
        if (!w.text.empty()) {
            work.push_back(std::move(w));
        }
    }
    std::ranges::stable_sort(work, [](const Work& a, const Work& b) { return a.start < b.start; });

    // 1. Extend into the gaps.
    for (std::size_t i = 0; i < work.size(); ++i) {
        Work& w = work[i];
        const Ticks wanted = need(w.text);
        if (w.end - w.start >= wanted) {
            continue;
        }
        Ticks endLimit = std::min(w.end + timing.maxExtend, w.start + timing.maxDuration);
        if (i + 1 < work.size()) {
            endLimit = std::min(endLimit, work[i + 1].start - timing.minGap);
        }
        const Ticks newEnd = std::max(w.end, std::min(endLimit, w.start + wanted));
        // The lead-in never reaches before the sequence starts (the first cue at 0 got a negative start).
        Ticks startLimit = std::max({w.start - timing.maxLead, newEnd - timing.maxDuration, Ticks{}});
        if (i > 0) {
            startLimit = std::max(startLimit, work[i - 1].end + timing.minGap);
        }
        const Ticks newStart = std::min(w.start, std::max(startLimit, newEnd - wanted));
        if (newEnd != w.end || newStart != w.start) {
            w.end = newEnd;
            w.start = newStart;
            w.extended = true;
        }
    }

    // 2. Merge with, or move words across to, a slower neighbour of the same speaker.
    const auto fast = [&](const Work& w) { return cpsOf(w.text, w.end - w.start) > limit + 1e-9; };
    const auto sameSpeaker = [](const Work& a, const Work& b) { return !a.speaker.isValid() || !b.speaker.isValid() || a.speaker == b.speaker; };
    for (std::size_t guard = 0; guard < 4 * work.size() + 4; ++guard) {
        bool changed = false;
        for (std::size_t i = 0; i < work.size() && !changed; ++i) {
            if (!fast(work[i])) {
                continue;
            }
            struct Option {
                std::size_t first = 0; ///< the earlier of the pair
                bool merge = false;
                std::string a;
                std::string b;
                Ticks boundary{};
                double worst = 0.0;
            };
            std::optional<Option> best;
            for (const std::size_t j : {i > 0 ? i - 1 : work.size(), i + 1}) {
                if (j >= work.size()) {
                    continue;
                }
                const std::size_t first = std::min(i, j);
                const Work& x = work[first];
                const Work& y = work[first + 1];
                if (!sameSpeaker(x, y) || y.start - x.end > timing.mergeGap) {
                    continue;
                }
                const double current = std::max(cpsOf(x.text, x.end - x.start), cpsOf(y.text, y.end - y.start));
                const std::string together = joinText(x.text, y.text, language);
                if (y.end - x.start <= timing.maxDuration && fits(together, language, rules, breaker)) {
                    const double merged = cpsOf(together, y.end - x.start);
                    if (merged < current && (!best || merged < best->worst)) {
                        best = Option{first, true, together, {}, {}, merged};
                    }
                    continue;
                }
                // Rebalance: the cut that makes the slower of the two parts as fast as possible.
                bool forced = false;
                const std::vector<std::size_t> points = cutPoints(together, language, breaker, &forced);
                if (forced) {
                    continue; // never cut inside a word for this
                }
                const double total = static_cast<double>(readingLength(together));
                for (const std::size_t p : points) {
                    std::string a = trimmed(std::string_view(together).substr(0, p));
                    std::string b = trimmed(std::string_view(together).substr(p));
                    if (a.empty() || b.empty()) {
                        continue;
                    }
                    const double share = static_cast<double>(readingLength(a)) / std::max(1.0, total);
                    const Ticks span = y.end - x.start - timing.minGap;
                    const Ticks boundary = x.start + ticksOf(seconds(span) * share);
                    const Ticks da = boundary - x.start;
                    const Ticks db = y.end - (boundary + timing.minGap);
                    if (da < timing.minDuration || db < timing.minDuration || da > timing.maxDuration || db > timing.maxDuration) {
                        continue;
                    }
                    const double worst = std::max(cpsOf(a, da), cpsOf(b, db));
                    if (worst + 0.5 < current && fits(a, language, rules, breaker) && fits(b, language, rules, breaker) &&
                        (!best || worst < best->worst)) {
                        best = Option{first, false, std::move(a), std::move(b), boundary, worst};
                    }
                }
            }
            if (!best) {
                continue;
            }
            Work& x = work[best->first];
            Work& y = work[best->first + 1];
            if (best->merge) {
                x.text = best->a;
                x.end = y.end;
                x.sources.insert(x.sources.end(), y.sources.begin(), y.sources.end());
                x.merged = true;
                x.extended = x.extended || y.extended;
                work.erase(work.begin() + static_cast<std::ptrdiff_t>(best->first + 1));
                ++stats.merged;
            } else {
                x.text = best->a;
                y.text = best->b;
                x.end = best->boundary;
                y.start = best->boundary + timing.minGap;
                x.rebalanced = true;
                y.rebalanced = true;
                ++stats.rebalanced;
            }
            changed = true;
        }
        if (!changed) {
            break;
        }
    }

    // 3. Lines, and splits of what does not fit.
    std::vector<TimedCue> out;
    for (const Work& w : work) {
        std::vector<std::pair<std::string, bool>> pieces{{w.text, false}};
        for (std::size_t parts = 2; parts <= 64; ++parts) {
            bool allFit = true;
            for (const auto& piece : pieces) {
                allFit = allFit && fits(piece.first, language, rules, breaker);
            }
            if (allFit) {
                break;
            }
            bool forced = false;
            const std::vector<std::string> split = splitText(w.text, parts, language, breaker, &forced);
            pieces.clear();
            for (const std::string& s : split) {
                pieces.emplace_back(s, forced);
            }
            if (split.size() < parts) {
                break; // no more cuts
            }
        }
        if (pieces.size() > 1) {
            stats.split += pieces.size() - 1;
        }
        const double total = std::max(1.0, [&] {
            double sum = 0.0;
            for (const auto& piece : pieces) {
                sum += static_cast<double>(readingLength(piece.first));
            }
            return sum;
        }());
        const Ticks gaps{timing.minGap.value * static_cast<std::int64_t>(pieces.size() - 1)};
        const Ticks available = std::max(Ticks{1}, w.end - w.start - gaps);
        Ticks at = w.start;
        for (std::size_t k = 0; k < pieces.size(); ++k) {
            TimedCue cue;
            cue.start = at;
            const bool last = k + 1 == pieces.size();
            cue.duration = last ? std::max(Ticks{1}, w.end - at)
                                : std::max(Ticks{1}, ticksOf(seconds(available) * static_cast<double>(readingLength(pieces[k].first)) / total));
            auto lines = breakCaptionLines(pieces[k].first, language, rules.maxCharsPerLine, rules.maxLines, breaker);
            if (lines) {
                std::string joined;
                for (std::size_t l = 0; l < lines->size(); ++l) {
                    joined += (l ? "\n" : "") + (*lines)[l];
                }
                cue.text = std::move(joined);
            } else {
                cue.text = pieces[k].first;
                cue.overflow = true;
            }
            cue.overflow = cue.overflow || pieces[k].second;
            cue.speaker = w.speaker;
            cue.sources = w.sources;
            if (!w.extended && !w.merged && !w.rebalanced && pieces.size() == 1) {
                ++stats.kept;
            }
            at = cue.end() + timing.minGap;
            out.push_back(std::move(cue));
        }
        if (w.extended) {
            ++stats.extended;
        }
    }

    // 4. The shortest cues stay up long enough to be noticed.
    for (std::size_t i = 0; i < out.size(); ++i) {
        TimedCue& cue = out[i];
        if (cue.duration >= timing.minDuration) {
            continue;
        }
        Ticks endLimit = cue.start + timing.minDuration;
        if (i + 1 < out.size()) {
            endLimit = std::min(endLimit, out[i + 1].start - timing.minGap);
        }
        if (endLimit > cue.end()) {
            cue.duration = endLimit - cue.start;
        }
        if (cue.duration < timing.minDuration) {
            // Backwards then, but never before the sequence starts: a negative start had the whole translation refused.
            Ticks startLimit = std::max(cue.end() - timing.minDuration, Ticks{});
            if (i > 0) {
                startLimit = std::max(startLimit, out[i - 1].end() + timing.minGap);
            }
            if (startLimit < cue.start) {
                cue.duration = cue.end() - startLimit;
                cue.start = startLimit;
            }
        }
    }

    for (TimedCue& cue : out) {
        cue.charsPerSecond = cpsOf(cue.text, cue.duration);
        cue.tooFast = cue.charsPerSecond > limit + 0.5;
        stats.tooFast += cue.tooFast ? 1 : 0;
        stats.maxCharsPerSecond = std::max(stats.maxCharsPerSecond, cue.charsPerSecond);
    }
    if (report) {
        *report = stats;
    }
    return out;
}

} // namespace wabcut::ai::translate
