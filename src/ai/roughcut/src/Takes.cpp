#include <wabcut/ai/roughcut/Takes.h>

#include <wabcut/command/ExecutionContext.h>
#include <wabcut/command/Registry.h>
#include <wabcut/model/EditOps.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <numeric>
#include <set>
#include <tuple>
#include <unordered_map>

namespace wabcut::ai::roughcut {

namespace {

struct Attempt {
    std::size_t source = 0;
    std::size_t first = 0; ///< words
    std::size_t last = 0;
    std::size_t unitBegin = 0;
    std::size_t unitEnd = 0;
    std::size_t spoken = 0; ///< non-filler words
    TimeRange range;
    double norm = 0.0;
};

std::span<const Unit> unitsOf(const SpeechCorpus& corpus, const Attempt& a) {
    const SpeechSource& s = corpus.sources[a.source];
    return {s.units.data() + a.unitBegin, a.unitEnd - a.unitBegin};
}

/// Words that carry the content of an English sentence less than the others (a substitution of these is not a
/// different line).
bool isFunctionWord(std::string_view w) {
    static const std::set<std::string_view> kWords{
        "a",    "an",   "the",  "and",   "or",   "but",  "so",    "to",    "of",   "in",    "on",   "at",   "for",  "with", "is",
        "are",  "was",  "were", "be",    "been", "it",   "its",   "this",  "that", "these", "those", "we",  "you",  "i",    "he",
        "she",  "they", "our",  "your",  "my",   "their", "im",   "ill",   "well", "were",  "youre", "its", "going", "gonna", "will",
        "would", "can", "could", "just", "really", "very", "about", "from", "as",  "do",    "dont", "not",  "have", "has",  "had"};
    return kWords.contains(w);
}

/// How many content words one attempt says where the other says another (a substitution in the diff).
std::size_t contentSubstitutions(std::span<const Unit> a, std::span<const Unit> b) {
    const std::vector<DiffRun> runs = diffUnits(a, b);
    std::size_t count = 0;
    for (std::size_t i = 0; i + 1 < runs.size(); ++i) {
        if (runs[i].kind != DiffRun::Kind::Deleted || runs[i + 1].kind != DiffRun::Kind::Inserted) {
            continue;
        }
        const auto content = [](std::span<const Unit> us, std::size_t from, std::size_t to) {
            for (std::size_t k = from; k < to; ++k) {
                if (us[k].word && us[k].text.size() >= 3 && !isFunctionWord(us[k].text)) {
                    return true;
                }
            }
            return false;
        };
        if (content(a, runs[i].aBegin, runs[i].aEnd) && content(b, runs[i + 1].bBegin, runs[i + 1].bEnd)) {
            ++count;
        }
    }
    return count;
}

double meaningFromCosine(double cosine) { return std::clamp((cosine - 0.3) / 0.5, 0.0, 1.0); }

class PairCache {
public:
    PairCache(const SpeechCorpus& corpus, const std::vector<Attempt>& attempts, const TakesSignals& inputs, const TakesOptions& options)
        : m_corpus(corpus), m_attempts(attempts), m_signals(inputs), m_options(options) {}

    /// Similarity of two attempts as attempts of one line (0 when they cannot be: too far apart, other recordings
    /// when that is off). `fragment` reports a cut-off start.
    double similarity(std::size_t i, std::size_t j, bool* fragment = nullptr) {
        if (i > j) {
            std::swap(i, j);
        }
        const auto key = std::pair{i, j};
        if (const auto it = m_cache.find(key); it != m_cache.end()) {
            if (fragment) {
                *fragment = it->second.second;
            }
            return it->second.first;
        }
        bool isFragment = false;
        const double sim = compute(i, j, isFragment);
        m_cache.emplace(key, std::pair{sim, isFragment});
        if (fragment) {
            *fragment = isFragment;
        }
        return sim;
    }

private:
    const std::optional<std::vector<float>>& embedding(std::size_t i) {
        auto [it, inserted] = m_embeddings.try_emplace(i);
        if (inserted && m_signals.embed) {
            const Attempt& a = m_attempts[i];
            it->second = m_signals.embed(m_corpus.sources[a.source].transcript.textOf(a.first, a.last));
        }
        return it->second;
    }

    double compute(std::size_t i, std::size_t j, bool& fragment) {
        const Attempt& a = m_attempts[i];
        const Attempt& b = m_attempts[j];
        if (a.source == b.source) {
            const Ticks gap = a.range.start < b.range.start ? b.range.start - a.range.end() : a.range.start - b.range.end();
            if (gap > m_options.maxGap) {
                return 0.0;
            }
        } else if (!m_options.acrossSources) {
            return 0.0;
        }
        const std::span<const Unit> ua = unitsOf(m_corpus, a);
        const std::span<const Unit> ub = unitsOf(m_corpus, b);
        const Alignment ab = align(ua, ub);
        const Alignment ba = align(ub, ua);
        double sim = (ab.quality() + ba.quality()) / 2.0;
        // A cut-off start: the shorter is (nearly) all inside the beginning of the longer, said just before it.
        const Attempt& shorter = a.spoken <= b.spoken ? a : b;
        const Attempt& longer = a.spoken <= b.spoken ? b : a;
        const Alignment& inside = a.spoken <= b.spoken ? ab : ba;
        if (shorter.spoken >= 2 && shorter.spoken * 10 <= longer.spoken * 7 && inside.coverage >= 0.85 && shorter.source == longer.source &&
            shorter.range.start < longer.range.start && longer.range.start - shorter.range.end() <= Ticks::fromSeconds(10)) {
            // It must align with the longer one's start (its first few units).
            const std::size_t startSlack = std::min<std::size_t>(3, longer.unitEnd - longer.unitBegin);
            if (inside.begin <= startSlack) {
                fragment = true;
                sim = std::max(sim, 0.75);
            }
        }
        if (!fragment && std::min(a.spoken, b.spoken) < m_options.minWords) {
            return 0.0; // short lines group only as false starts
        }
        // Short lines that swap a content word are different lines ("the first version" / "the second version").
        if (!fragment && std::max(a.spoken, b.spoken) < 10) {
            sim -= 0.15 * static_cast<double>(contentSubstitutions(ua, ub));
        }
        if (m_signals.embed && !fragment && sim >= 0.4 && sim < m_options.minSimilarity) {
            const auto& ea = embedding(i);
            const auto& eb = embedding(j);
            if (ea && eb && ea->size() == eb->size()) {
                double dot = 0.0;
                for (std::size_t k = 0; k < ea->size(); ++k) {
                    dot += static_cast<double>((*ea)[k]) * (*eb)[k];
                }
                sim = std::max(sim, 0.5 * sim + 0.5 * meaningFromCosine(dot));
            }
        }
        return std::clamp(sim, 0.0, 1.0);
    }

    const SpeechCorpus& m_corpus;
    const std::vector<Attempt>& m_attempts;
    const TakesSignals& m_signals;
    const TakesOptions& m_options;
    std::map<std::pair<std::size_t, std::size_t>, std::pair<double, bool>> m_cache;
    std::map<std::size_t, std::optional<std::vector<float>>> m_embeddings;
};

std::vector<DiffWord> wordDiff(const SpeechCorpus& corpus, const WordSpan& from, const WordSpan& to) {
    const SpeechSource& sa = corpus.sources[from.source];
    const SpeechSource& sb = corpus.sources[to.source];
    const std::span<const Unit> ua(sa.units.data() + sa.unitStart[from.first], sa.unitStart[from.last + 1] - sa.unitStart[from.first]);
    const std::span<const Unit> ub(sb.units.data() + sb.unitStart[to.first], sb.unitStart[to.last + 1] - sb.unitStart[to.first]);
    std::vector<DiffWord> out;
    for (const DiffRun& run : diffUnits(ua, ub)) {
        const bool fromA = run.kind == DiffRun::Kind::Deleted;
        const SpeechSource& s = fromA ? sa : sb;
        const std::span<const Unit> us = fromA ? ua : ub;
        const std::size_t begin = fromA ? run.aBegin : run.bBegin;
        const std::size_t end = fromA ? run.aEnd : run.bEnd;
        if (begin >= end) {
            continue;
        }
        // The words those units came from, as said (a Thai word is one entry even when it is several units).
        std::vector<std::uint32_t> words;
        for (std::size_t k = begin; k < end; ++k) {
            if (words.empty() || words.back() != us[k].source) {
                words.push_back(us[k].source);
            }
        }
        DiffWord w;
        w.kind = run.kind == DiffRun::Kind::Equal ? DiffWord::Kind::Equal : fromA ? DiffWord::Kind::Deleted : DiffWord::Kind::Inserted;
        w.text = s.transcript.textOf(words.front(), words.back());
        out.push_back(std::move(w));
    }
    return out;
}

} // namespace

bool TakeGroup::improvable() const {
    if (onTimeline == 0 || takes.empty()) {
        return false;
    }
    return !takes.front().onTimeline || onTimeline > 1;
}

TakesReport findTakes(const SpeechCorpus& corpus, const TakesSignals& inputs, const TakesOptions& options) {
    const auto started = std::chrono::steady_clock::now();
    TakesReport report;
    report.sources = corpus.sources.size();
    // 1. Attempts: sentences of two or more spoken words.
    std::vector<Attempt> attempts;
    std::vector<FeatureBag> bags;
    for (std::size_t s = 0; s < corpus.sources.size(); ++s) {
        const SpeechSource& source = corpus.sources[s];
        for (const shorts::Sentence& sentence : source.sentences) {
            Attempt a;
            a.source = s;
            a.first = sentence.first;
            a.last = sentence.last;
            for (std::size_t w = a.first; w <= a.last; ++w) {
                a.spoken += source.filler[w] ? 0u : 1u;
            }
            if (a.spoken < 2) {
                continue;
            }
            a.unitBegin = source.unitStart[a.first];
            a.unitEnd = source.unitStart[a.last + 1];
            a.range = spanRange(corpus, WordSpan{s, a.first, a.last});
            bags.push_back(featuresOf(unitsOf(corpus, a)));
            attempts.push_back(a);
        }
    }
    report.sentences = attempts.size();
    // 2. Candidate pairs from an IDF-weighted index, verified by alignment.
    std::unordered_map<std::uint64_t, std::uint32_t> df;
    for (const FeatureBag& bag : bags) {
        for (const auto& feature : bag.features) {
            ++df[feature.first];
        }
    }
    const double n = static_cast<double>(bags.size());
    const auto idf = [&](std::uint64_t f) { return std::log((n + 1.0) / (static_cast<double>(df[f]) + 1.0)) + 1.0; };
    std::unordered_map<std::uint64_t, std::vector<std::pair<std::uint32_t, double>>> postings;
    for (std::size_t i = 0; i < bags.size(); ++i) {
        double norm = 0.0;
        for (const auto& [feature, count] : bags[i].features) {
            const double w = static_cast<double>(count) * idf(feature);
            norm += w * w;
            postings[feature].emplace_back(static_cast<std::uint32_t>(i), w);
        }
        attempts[i].norm = std::sqrt(norm);
    }
    PairCache cache(corpus, attempts, inputs, options);
    struct Pair {
        std::size_t a = 0;
        std::size_t b = 0;
        double sim = 0.0;
        bool fragment = false;
    };
    std::vector<Pair> pairs;
    std::vector<double> accumulated(attempts.size(), 0.0);
    std::vector<std::uint32_t> touched;
    for (std::size_t i = 0; i < attempts.size(); ++i) {
        for (const auto& [feature, count] : bags[i].features) {
            const double wq = static_cast<double>(count) * idf(feature);
            for (const auto& [j, wd] : postings[feature]) {
                if (j <= i) {
                    continue;
                }
                if (accumulated[j] == 0.0) {
                    touched.push_back(j);
                }
                accumulated[j] += wq * wd;
            }
        }
        for (const std::uint32_t j : touched) {
            const double cos = attempts[i].norm > 0.0 && attempts[j].norm > 0.0 ? accumulated[j] / (attempts[i].norm * attempts[j].norm) : 0.0;
            accumulated[j] = 0.0;
            if (cos < 0.2) {
                continue;
            }
            bool fragment = false;
            const double sim = cache.similarity(i, j, &fragment);
            if (sim >= options.minSimilarity || fragment) {
                pairs.push_back({i, j, sim, fragment});
            }
        }
        touched.clear();
    }
    // 3. Groups: the most similar pairs first; an attempt joins only when it is like every attempt already there
    //    (false starts need only be the start of one).
    std::ranges::sort(pairs, [](const Pair& x, const Pair& y) {
        return x.sim != y.sim ? x.sim > y.sim : std::pair{x.a, x.b} < std::pair{y.a, y.b};
    });
    std::vector<std::size_t> groupOf(attempts.size(), static_cast<std::size_t>(-1));
    std::vector<std::vector<std::size_t>> members;
    std::vector<bool> isFragment(attempts.size(), false);
    const double joinFloor = options.minSimilarity * 0.85;
    const auto fits = [&](std::size_t candidate, const std::vector<std::size_t>& group) {
        if (isFragment[candidate]) {
            return true;
        }
        return std::ranges::all_of(group, [&](std::size_t m) { return isFragment[m] || cache.similarity(candidate, m) >= joinFloor; });
    };
    for (const Pair& p : pairs) {
        const std::size_t ga = groupOf[p.a];
        const std::size_t gb = groupOf[p.b];
        if (p.fragment) {
            const Attempt& x = attempts[p.a];
            const Attempt& y = attempts[p.b];
            isFragment[x.spoken <= y.spoken ? p.a : p.b] = true;
        }
        const std::size_t none = static_cast<std::size_t>(-1);
        if (ga == none && gb == none) {
            groupOf[p.a] = groupOf[p.b] = members.size();
            members.push_back({p.a, p.b});
        } else if (ga != none && gb == none) {
            if (members[ga].size() < options.maxTakesPerGroup && fits(p.b, members[ga])) {
                groupOf[p.b] = ga;
                members[ga].push_back(p.b);
            }
        } else if (ga == none && gb != none) {
            if (members[gb].size() < options.maxTakesPerGroup && fits(p.a, members[gb])) {
                groupOf[p.a] = gb;
                members[gb].push_back(p.a);
            }
        } else if (ga != gb && members[ga].size() + members[gb].size() <= options.maxTakesPerGroup) {
            const bool ok = std::ranges::all_of(members[gb], [&](std::size_t m) { return fits(m, members[ga]); });
            if (ok) {
                for (const std::size_t m : members[gb]) {
                    groupOf[m] = ga;
                    members[ga].push_back(m);
                }
                members[gb].clear();
            }
        }
    }
    // 4. Each group: fold media attempts that are a sequence attempt's own words, judge, rank, diff.
    std::vector<TakeGroup> groups;
    std::vector<std::pair<WordSpan, std::size_t>> toMeasure; // span, group index
    for (std::vector<std::size_t>& group : members) {
        if (group.size() < 2) {
            continue;
        }
        std::ranges::sort(group, [&](std::size_t x, std::size_t y) {
            return std::pair{attempts[x].source, attempts[x].first} < std::pair{attempts[y].source, attempts[y].first};
        });
        TakeGroup g;
        for (const std::size_t m : group) {
            const Attempt& a = attempts[m];
            Take t;
            t.span = WordSpan{a.source, a.first, a.last};
            t.range = a.range;
            t.media = mediaRangeOf(corpus, t.span);
            t.onTimeline = corpus.sources[a.source].kind == SpeechSource::Kind::Sequence;
            t.sourceName = corpus.sources[a.source].name;
            t.text = spanText(corpus, t.span);
            g.takes.push_back(std::move(t));
        }
        std::vector<Take> kept;
        for (Take& t : g.takes) {
            if (!t.onTimeline && t.media) {
                const bool played = std::ranges::any_of(g.takes, [&](const Take& p) {
                    if (!p.onTimeline || !p.media || p.media->media != t.media->media || p.media->audioStream != t.media->audioStream) {
                        return false;
                    }
                    const Ticks overlap = std::min(p.media->source.end(), t.media->source.end()) - std::max(p.media->source.start, t.media->source.start);
                    return overlap.value * 2 > std::min(p.media->source.duration, t.media->source.duration).value;
                });
                if (played) {
                    continue;
                }
            }
            kept.push_back(std::move(t));
        }
        if (kept.size() < 2) {
            continue;
        }
        g.takes = std::move(kept);
        g.onTimeline = static_cast<std::size_t>(std::ranges::count_if(g.takes, [](const Take& t) { return t.onTimeline; }));
        // The fullest attempt is the reference for completeness.
        std::size_t reference = 0;
        for (std::size_t k = 1; k < g.takes.size(); ++k) {
            const auto spoken = [&](const Take& t) {
                const SpeechSource& s = corpus.sources[t.span.source];
                std::size_t c = 0;
                for (std::size_t w = t.span.first; w <= t.span.last; ++w) {
                    c += s.filler[w] ? 0u : 1u;
                }
                return c;
            };
            if (spoken(g.takes[k]) > spoken(g.takes[reference])) {
                reference = k;
            }
        }
        const SpeechSource& rs = corpus.sources[g.takes[reference].span.source];
        const std::span<const Unit> referenceUnits(rs.units.data() + rs.unitStart[g.takes[reference].span.first],
                                                   rs.unitStart[g.takes[reference].span.last + 1] - rs.unitStart[g.takes[reference].span.first]);
        for (std::size_t k = 0; k < g.takes.size(); ++k) {
            Take& t = g.takes[k];
            const SpeechSource& s = corpus.sources[t.span.source];
            const std::span<const Unit> us(s.units.data() + s.unitStart[t.span.first], s.unitStart[t.span.last + 1] - s.unitStart[t.span.first]);
            t.completeness = k == reference ? 1.0 : align(referenceUnits, us).coverage;
            t.quality = assessSpan(corpus, t.span, t.completeness, options.corpus);
            toMeasure.emplace_back(t.span, groups.size());
        }
        // The weakest link between attempts that are not false starts.
        double weakest = 1.0;
        for (std::size_t x = 0; x < group.size(); ++x) {
            for (std::size_t y = x + 1; y < group.size(); ++y) {
                if (!isFragment[group[x]] && !isFragment[group[y]]) {
                    weakest = std::min(weakest, cache.similarity(group[x], group[y]));
                }
            }
        }
        g.confidence = weakest;
        groups.push_back(std::move(g));
    }
    if (inputs.audio) {
        std::vector<edit::AudioEnergyProbe::Range> ranges;
        for (const auto& measured : toMeasure) {
            const auto r = audioRanges(corpus, measured.first);
            ranges.insert(ranges.end(), r.begin(), r.end());
        }
        inputs.audio->prefetch(ranges);
    }
    std::vector<double> levels;
    for (TakeGroup& g : groups) {
        for (Take& t : g.takes) {
            if (inputs.audio) {
                measureAudio(corpus, t.span, *inputs.audio, t.quality);
                if (t.quality.rmsDb && std::isfinite(*t.quality.rmsDb)) {
                    levels.push_back(*t.quality.rmsDb);
                }
            }
        }
    }
    std::optional<double> medianLevel;
    if (!levels.empty()) {
        std::ranges::sort(levels);
        medianLevel = levels[levels.size() / 2];
    }
    for (TakeGroup& g : groups) {
        for (Take& t : g.takes) {
            scoreTake(t.quality, medianLevel, corpus.sources[t.span.source].wordsPerSecond);
        }
        std::ranges::stable_sort(g.takes, [](const Take& x, const Take& y) {
            if (x.quality.score != y.quality.score) {
                return x.quality.score > y.quality.score;
            }
            return std::pair{x.span.source, x.span.first} < std::pair{y.span.source, y.span.first};
        });
        g.text = g.takes.front().text;
        for (std::size_t k = 1; k < g.takes.size(); ++k) {
            g.takes[k].diff = wordDiff(corpus, g.takes.front().span, g.takes[k].span);
        }
    }
    // Order: groups on the timeline by where they are first heard, then the rest by source and time.
    std::ranges::stable_sort(groups, [](const TakeGroup& x, const TakeGroup& y) {
        const auto key = [](const TakeGroup& g) {
            Ticks first = Ticks::max();
            std::size_t source = static_cast<std::size_t>(-1);
            for (const Take& t : g.takes) {
                if (t.onTimeline) {
                    first = std::min(first, t.range.start);
                }
            }
            if (first == Ticks::max()) {
                for (const Take& t : g.takes) {
                    if (std::pair{t.span.source, t.range.start} < std::pair{source, first}) {
                        source = t.span.source;
                        first = t.range.start;
                    }
                }
                return std::tuple{1, source, first};
            }
            return std::tuple{0, std::size_t{0}, first};
        };
        return key(x) < key(y);
    });
    if (groups.size() > options.maxGroups) {
        report.warnings.push_back("only the first " + std::to_string(options.maxGroups) + " groups of takes are listed");
        groups.resize(options.maxGroups);
    }
    for (std::size_t i = 0; i < groups.size(); ++i) {
        groups[i].id = i;
    }
    report.groups = std::move(groups);
    report.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    return report;
}

std::optional<MediaRange> insertRangeOf(const SpeechCorpus& corpus, const Take& take, Ticks handle) {
    return mediaRangeWithHandles(corpus, take.span, handle);
}

std::expected<std::vector<TakeEdit>, Error> planUseTake(const SpeechCorpus& corpus, const TakeGroup& group, std::size_t take,
                                                        const TakeEditOptions& options) {
    if (take >= group.takes.size()) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "the group has " + std::to_string(group.takes.size()) + " takes"));
    }
    std::vector<const Take*> present;
    for (const Take& t : group.takes) {
        if (t.onTimeline) {
            present.push_back(&t);
        }
    }
    if (present.empty()) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "no take of \"" + group.text.substr(0, 60) +
                                                                     "\" is in the sequence: insert the one you want instead"));
    }
    std::ranges::sort(present, [](const Take* a, const Take* b) { return a->range.start < b->range.start; });
    const Take& chosen = group.takes[take];
    const std::string label = "\"" + (chosen.text.size() > 48 ? chosen.text.substr(0, 45) + "..." : chosen.text) + "\"";
    std::vector<TakeEdit> edits;
    if (chosen.onTimeline) {
        for (const Take* p : present) {
            if (p != &chosen) {
                edits.push_back(TakeEdit{p->span.first, p->span.last, std::nullopt, "remove another take of " + label});
            }
        }
        return edits;
    }
    const auto range = insertRangeOf(corpus, chosen, options.handle);
    if (!range) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "that take does not play one stretch of one media item"));
    }
    edits.push_back(TakeEdit{present.front()->span.first, present.front()->span.last, range, "use the take from '" + chosen.sourceName + "' for " + label});
    for (std::size_t k = 1; k < present.size(); ++k) {
        edits.push_back(TakeEdit{present[k]->span.first, present[k]->span.last, std::nullopt, "remove another take of " + label});
    }
    return edits;
}

ProjectState applyTakeEdits(const ProjectState& state, SequenceId sequence, const edit::SequenceTranscript& words, std::vector<TakeEdit> edits,
                            const TakeEditOptions& options, const command::CommandRegistry& registry, command::ExecutionControl* control,
                            command::CommandServices* services) {
    using command::ErrorCode;
    using command::fail;
    const Sequence* seq = state.findSequence(sequence);
    if (!seq) {
        fail(ErrorCode::NotFound, "no sequence " + std::to_string(sequence.value));
    }
    struct Planned {
        TakeEdit edit;
        edit::EditPlan plan;
        TrackId audioTrack;
        std::optional<TrackId> videoTrack;
    };
    std::vector<Planned> planned;
    for (TakeEdit& e : edits) {
        if (e.firstWord > e.lastWord || e.lastWord >= words.size()) {
            fail(ErrorCode::InvalidParams, "a take's words are not in the sequence's transcript (it changed: find the takes again)");
        }
        edit::DeleteRequest request;
        request.words.push_back({e.firstWord, e.lastWord});
        if (e.with) {
            request.options.padBefore = options.replacePad;
            request.options.padAfter = options.replacePad;
        } else {
            request.options = options.removeCut;
            if (e.lastWord + 1 < words.size() && words[e.lastWord + 1].clip == words[e.lastWord].clip) {
                request.gapsAfter.push_back(e.lastWord);
            }
        }
        auto plan = edit::planDelete(words, state, request);
        if (!plan) {
            fail(ErrorCode::EditRejected, e.label + ": " + plan.error().message);
        }
        if (plan->cuts.empty()) {
            continue;
        }
        const edit::TimelineWord& w = words[e.firstWord];
        Planned p{std::move(e), std::move(*plan), w.track, std::nullopt};
        if (w.linkGroup.isValid()) {
            for (const TrackPtr& t : seq->videoTracks) {
                for (const Clip& c : t->clips) {
                    if (c.linkGroup == w.linkGroup) {
                        p.videoTrack = t->id;
                    }
                }
            }
        }
        planned.push_back(std::move(p));
    }
    // Earlier edits must end where later ones start (their cuts were planned on the same state).
    std::ranges::sort(planned, [](const Planned& a, const Planned& b) { return a.plan.cuts.front().range.start < b.plan.cuts.front().range.start; });
    for (std::size_t k = 0; k + 1 < planned.size(); ++k) {
        edit::PlannedCut& last = planned[k].plan.cuts.back();
        const Ticks nextStart = planned[k + 1].plan.cuts.front().range.start;
        if (last.range.end() > nextStart) {
            last.range = TimeRange::fromStartEnd(last.range.start, std::max(last.range.start, nextStart));
        }
    }
    const std::optional<Ticks> markIn = seq->markIn;
    const std::optional<Ticks> markOut = seq->markOut;
    const Ticks frame{seq->frameRate.ticksPerUnit()};
    ProjectState working = state;
    for (auto it = planned.rbegin(); it != planned.rend(); ++it) {
        std::erase_if(it->plan.cuts, [](const edit::PlannedCut& c) { return c.range.duration <= Ticks{}; });
        if (!it->plan.cuts.empty()) {
            working = edit::runInline(registry, working, edit::toCommandBatch(it->plan), control, services);
        }
        if (!it->edit.with) {
            continue;
        }
        const MediaRange& range = *it->edit.with;
        const MediaItem* media = working.findMedia(range.media);
        if (!media) {
            fail(ErrorCode::NotFound, it->edit.label + ": its media is no longer in the project");
        }
        Ticks length{(range.source.duration.value + frame.value - 1) / frame.value * frame.value};
        if (const auto extent = media->sourceExtent(StreamRef{StreamKind::Audio, range.audioStream}); extent && range.source.start + length > *extent) {
            length = Ticks{(*extent - range.source.start).value / frame.value * frame.value};
        }
        if (length <= Ticks{}) {
            fail(ErrorCode::EditRejected, it->edit.label + ": the take is shorter than a frame");
        }
        wabcut::edit::SourceClip source;
        source.media = range.media;
        source.sourceRange = TimeRange{range.source.start, length};
        if (it->videoTrack && media->info.hasVideo()) {
            source.patches.push_back({StreamRef{StreamKind::Video, 0}, *it->videoTrack});
        }
        source.patches.push_back({StreamRef{StreamKind::Audio, range.audioStream}, it->audioTrack});
        const Ticks at = it->plan.cuts.empty() ? words[it->edit.firstWord].timeline.start : it->plan.cuts.front().range.start;
        auto inserted = wabcut::edit::insertEdit(working, sequence, source, at);
        if (!inserted) {
            fail(ErrorCode::EditRejected, it->edit.label + ": " + inserted.error().message);
        }
        working = std::move(inserted->state);
    }
    const Sequence* after = working.findSequence(sequence);
    if (after && (after->markIn != markIn || after->markOut != markOut)) {
        auto next = std::make_shared<Sequence>(*after);
        next->markIn = markIn;
        next->markOut = markOut;
        working = working.withSequence(std::move(next));
    }
    return working;
}

} // namespace wabcut::ai::roughcut
