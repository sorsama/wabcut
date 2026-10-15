#include <wabcut/ai/roughcut/RoughCut.h>

#include <wabcut/ai/roughcut/Outline.h>

#include <wabcut/model/EditOps.h>
#include <wabcut/model/ProjectOps.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <unordered_map>

namespace wabcut::ai::roughcut {

using Json = nlohmann::json;

namespace {

struct IndexedSentence {
    std::uint32_t source = 0;
    std::uint32_t sentence = 0; ///< index into the source's sentences
    double norm = 0.0;          ///< of its IDF-weighted bag
};

/// The retrieval index over every sentence of the media sources.
class SentenceIndex {
public:
    explicit SentenceIndex(const SpeechCorpus& corpus) {
        std::vector<FeatureBag> bags;
        for (std::size_t s = 0; s < corpus.sources.size(); ++s) {
            const SpeechSource& source = corpus.sources[s];
            if (source.kind != SpeechSource::Kind::Media) {
                continue;
            }
            m_firstOfSource.push_back({static_cast<std::uint32_t>(s), static_cast<std::uint32_t>(m_sentences.size())});
            for (std::size_t i = 0; i < source.sentences.size(); ++i) {
                const shorts::Sentence& sentence = source.sentences[i];
                const std::span<const Unit> us(source.units.data() + source.unitStart[sentence.first],
                                               source.unitStart[sentence.last + 1] - source.unitStart[sentence.first]);
                bags.push_back(featuresOf(us));
                m_sentences.push_back({static_cast<std::uint32_t>(s), static_cast<std::uint32_t>(i), 0.0});
            }
        }
        std::unordered_map<std::uint64_t, std::uint32_t> df;
        for (const FeatureBag& bag : bags) {
            for (const auto& feature : bag.features) {
                ++df[feature.first];
            }
        }
        const double n = static_cast<double>(bags.size());
        m_unseenIdf = std::log(n + 1.0) + 1.0;
        for (const auto& [feature, count] : df) {
            m_idf.emplace(feature, std::log((n + 1.0) / (static_cast<double>(count) + 1.0)) + 1.0);
        }
        m_vectors.resize(bags.size());
        for (std::size_t i = 0; i < bags.size(); ++i) {
            double norm = 0.0;
            for (const auto& [feature, count] : bags[i].features) {
                const double w = static_cast<double>(count) * m_idf.at(feature);
                norm += w * w;
                m_postings[feature].emplace_back(static_cast<std::uint32_t>(i), w);
                m_vectors[i].emplace_back(feature, w);
            }
            std::ranges::sort(m_vectors[i]);
            m_sentences[i].norm = std::sqrt(norm);
        }
    }

    /// The dot product of two sentences' weighted vectors (for the exact norm of a window of sentences).
    double dot(std::uint32_t a, std::uint32_t b) const {
        const auto& x = m_vectors[a];
        const auto& y = m_vectors[b];
        double sum = 0.0;
        for (std::size_t i = 0, j = 0; i < x.size() && j < y.size();) {
            if (x[i].first < y[j].first) {
                ++i;
            } else if (y[j].first < x[i].first) {
                ++j;
            } else {
                sum += x[i++].second * y[j++].second;
            }
        }
        return sum;
    }

    double idf(std::uint64_t feature) const {
        const auto it = m_idf.find(feature);
        return it == m_idf.end() ? m_unseenIdf : it->second;
    }
    const std::vector<std::pair<std::uint32_t, double>>* postings(std::uint64_t feature) const {
        const auto it = m_postings.find(feature);
        return it == m_postings.end() ? nullptr : &it->second;
    }
    std::span<const IndexedSentence> sentences() const { return m_sentences; }

private:
    std::vector<IndexedSentence> m_sentences;
    std::vector<std::vector<std::pair<std::uint64_t, double>>> m_vectors; ///< per sentence, sorted by feature
    std::vector<std::pair<std::uint32_t, std::uint32_t>> m_firstOfSource;
    std::unordered_map<std::uint64_t, double> m_idf;
    std::unordered_map<std::uint64_t, std::vector<std::pair<std::uint32_t, double>>> m_postings;
    double m_unseenIdf = 1.0;
};

struct Window {
    std::uint32_t first = 0; ///< global sentence indices [first, last]
    std::uint32_t last = 0;
    double score = 0.0;
};

/// Maps an embedder's cosine to 0..1: unrelated texts sit near 0.3, paraphrases above 0.8.
double meaningFromCosine(double cosine) { return std::clamp((cosine - 0.3) / 0.5, 0.0, 1.0); }

double dotf(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) {
        return 0.0;
    }
    double sum = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        sum += static_cast<double>(a[i]) * b[i];
    }
    return sum;
}

struct Candidate {
    BeatTake take;
    double lexical = 0.0;
};

std::size_t overlapWords(const WordSpan& a, const WordSpan& b) {
    if (a.source != b.source || a.last < b.first || b.last < a.first) {
        return 0;
    }
    return std::min(a.last, b.last) - std::max(a.first, b.first) + 1;
}

std::string truncateUtf8(std::string_view text, std::size_t maxBytes) {
    if (text.size() <= maxBytes) {
        return std::string(text);
    }
    std::size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return std::string(text.substr(0, cut)) + "\xE2\x80\xA6";
}

std::size_t blemishes(const TakeQuality& q) { return q.fillers + q.falseStarts + q.hesitations; }

} // namespace

std::vector<std::vector<WordSpan>> closestSentences(const SpeechCorpus& corpus, std::span<const std::string> texts, std::size_t perText) {
    const SentenceIndex index(corpus);
    const std::span<const IndexedSentence> sentences = index.sentences();
    std::vector<std::vector<WordSpan>> out;
    std::vector<double> accumulated(sentences.size(), 0.0);
    std::vector<std::uint32_t> touched;
    for (const std::string& text : texts) {
        const FeatureBag bag = featuresOf(units(text));
        double norm = 0.0;
        for (const auto& [feature, count] : bag.features) {
            const double wq = static_cast<double>(count) * index.idf(feature);
            norm += wq * wq;
            if (const auto* list = index.postings(feature)) {
                for (const auto& [sentence, ws] : *list) {
                    if (accumulated[sentence] == 0.0) {
                        touched.push_back(sentence);
                    }
                    accumulated[sentence] += wq * ws;
                }
            }
        }
        norm = std::sqrt(norm);
        std::vector<std::pair<double, std::uint32_t>> ranked;
        for (const std::uint32_t i : touched) {
            const double score = norm > 0.0 && sentences[i].norm > 0.0 ? accumulated[i] / (norm * sentences[i].norm) : 0.0;
            ranked.emplace_back(score, i);
            accumulated[i] = 0.0;
        }
        touched.clear();
        std::ranges::sort(ranked, [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
        std::vector<WordSpan> spans;
        for (std::size_t k = 0; k < ranked.size() && k < perText; ++k) {
            const IndexedSentence& is = sentences[ranked[k].second];
            const shorts::Sentence& sentence = corpus.sources[is.source].sentences[is.sentence];
            spans.push_back(WordSpan{is.source, sentence.first, sentence.last});
        }
        out.push_back(std::move(spans));
    }
    return out;
}

std::string_view toString(TakePreference preference) {
    switch (preference) {
    case TakePreference::Cleanest:
        return "cleanest";
    case TakePreference::First:
        return "first";
    case TakePreference::Last:
        return "last";
    case TakePreference::Best:
        break;
    }
    return "best";
}

std::optional<TakePreference> takePreferenceFromString(std::string_view text) {
    for (const TakePreference p : {TakePreference::Best, TakePreference::Cleanest, TakePreference::First, TakePreference::Last}) {
        if (text == toString(p)) {
            return p;
        }
    }
    return std::nullopt;
}

std::size_t RoughCutPlan::found() const {
    return static_cast<std::size_t>(std::ranges::count_if(beats, [](const BeatPlan& b) { return b.found(); }));
}

std::size_t preferredTake(const std::vector<BeatTake>& takes, TakePreference preference) {
    if (takes.empty()) {
        return 0;
    }
    std::size_t pick = 0;
    for (std::size_t i = 1; i < takes.size(); ++i) {
        const BeatTake& a = takes[i];
        const BeatTake& b = takes[pick];
        const auto order = [](const BeatTake& t) { return std::pair{t.span.source, t.media.source.start}; };
        bool better = false;
        switch (preference) {
        case TakePreference::Best:
            better = false; // ranked already
            break;
        case TakePreference::Cleanest:
            better = blemishes(a.quality) < blemishes(b.quality);
            break;
        case TakePreference::First:
            better = order(a) < order(b);
            break;
        case TakePreference::Last:
            better = order(b) < order(a);
            break;
        }
        if (better) {
            pick = i;
        }
    }
    return pick;
}

RoughCutPlan planRoughCut(const SpeechCorpus& corpus, const Script& script, const RoughCutSignals& inputs, const RoughCutOptions& options) {
    const auto started = std::chrono::steady_clock::now();
    RoughCutPlan plan;
    plan.script = script;
    for (const SpeechSource& source : corpus.sources) {
        if (source.kind == SpeechSource::Kind::Media) {
            ++plan.sources;
            plan.words += source.transcript.size();
        }
    }
    const SentenceIndex index(corpus);
    const std::span<const IndexedSentence> sentences = index.sentences();
    std::vector<double> accumulated(sentences.size(), 0.0);
    std::vector<std::uint32_t> touched;
    std::map<std::string, std::optional<std::vector<float>>> embeddings;
    const auto embed = [&](const std::string& text) -> const std::optional<std::vector<float>>& {
        auto [it, inserted] = embeddings.try_emplace(text);
        if (inserted && inputs.embed) {
            it->second = inputs.embed(text);
        }
        return it->second;
    };

    std::vector<std::vector<Candidate>> perBeat(script.beats.size());
    for (std::size_t b = 0; b < script.beats.size(); ++b) {
        const ScriptBeat& beat = script.beats[b];
        const std::vector<Unit> query = units(beat.text);
        if (query.empty()) {
            continue;
        }
        // 1. Retrieval: IDF-weighted feature overlap per sentence, then windows of consecutive sentences.
        const FeatureBag bag = featuresOf(query);
        double queryNorm = 0.0;
        for (const auto& [feature, count] : bag.features) {
            const double wq = static_cast<double>(count) * index.idf(feature);
            queryNorm += wq * wq;
            if (const auto* list = index.postings(feature)) {
                for (const auto& [sentence, ws] : *list) {
                    if (accumulated[sentence] == 0.0) {
                        touched.push_back(sentence);
                    }
                    accumulated[sentence] += wq * ws;
                }
            }
        }
        queryNorm = std::sqrt(queryNorm);
        std::vector<Window> windows;
        const std::size_t queryWords = query.size();
        // Seeds: the sentences that match best on their own (common words touch nearly every sentence).
        std::vector<std::pair<double, std::uint32_t>> seeds;
        seeds.reserve(touched.size());
        for (const std::uint32_t i : touched) {
            const double alone = sentences[i].norm > 0.0 && queryNorm > 0.0 ? accumulated[i] / (queryNorm * sentences[i].norm) : 0.0;
            seeds.emplace_back(alone, i);
        }
        const std::size_t seedCount = std::min(seeds.size(), std::max<std::size_t>(64, 4 * options.candidatesPerBeat));
        std::ranges::partial_sort(seeds, seeds.begin() + static_cast<std::ptrdiff_t>(seedCount),
                                  [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
        seeds.resize(seedCount);
        for (const auto& [alone, i] : seeds) {
            Window best{i, i, 0.0};
            const std::uint32_t source = sentences[i].source;
            const std::uint32_t reach = static_cast<std::uint32_t>(std::max<std::size_t>(options.maxSentencesPerWindow, 1));
            const std::uint32_t lo = i >= reach - 1 ? i - (reach - 1) : 0;
            for (std::uint32_t first = lo; first <= i; ++first) {
                if (sentences[first].source != source) {
                    continue;
                }
                double sum = 0.0;
                double norm2 = 0.0; // of the window's summed vector, exactly: two takes of one line are no better than one
                std::size_t windowUnits = 0;
                for (std::uint32_t last = first; last < sentences.size() && last < first + reach; ++last) {
                    if (sentences[last].source != source) {
                        break;
                    }
                    sum += accumulated[last];
                    norm2 += sentences[last].norm * sentences[last].norm;
                    for (std::uint32_t k = first; k < last; ++k) {
                        norm2 += 2.0 * index.dot(k, last);
                    }
                    const SpeechSource& src = corpus.sources[source];
                    const shorts::Sentence& sentence = src.sentences[sentences[last].sentence];
                    windowUnits += src.unitStart[sentence.last + 1] - src.unitStart[sentence.first];
                    if (last < i) {
                        continue;
                    }
                    if (windowUnits > 3 * queryWords + 12 && last > first) {
                        break;
                    }
                    const double score = norm2 > 0.0 && queryNorm > 0.0 ? sum / (queryNorm * std::sqrt(norm2)) : 0.0;
                    // The fewest sentences that say the line: a longer window must do clearly better, a shorter one wins a tie.
                    const std::uint32_t length = last - first;
                    const std::uint32_t bestLength = best.last - best.first;
                    const double needed = length > bestLength ? 1e-6 : length < bestLength ? -1e-9 : 1e-12;
                    if (score > best.score + needed) {
                        best = Window{first, last, score};
                    }
                }
            }
            if (best.score > 0.0) {
                windows.push_back(best);
            }
        }
        for (const std::uint32_t i : touched) {
            accumulated[i] = 0.0;
        }
        touched.clear();
        std::ranges::sort(windows, [](const Window& a, const Window& b) {
            return a.score != b.score ? a.score > b.score : std::pair{a.first, a.last} < std::pair{b.first, b.last};
        });
        windows.erase(std::unique(windows.begin(), windows.end(),
                                  [](const Window& a, const Window& b) { return a.first == b.first && a.last == b.last; }),
                      windows.end());
        // Non-maximum suppression: a window overlapping better ones keeps only its sentences they do not hold (another
        // take said right after the first may be there), or nothing.
        std::vector<Window> kept;
        for (const Window& w : windows) {
            const auto covered = [&](std::uint32_t s) { return std::ranges::any_of(kept, [&](const Window& k) { return s >= k.first && s <= k.last; }); };
            std::optional<Window> run;
            std::optional<Window> longest;
            for (std::uint32_t s = w.first; s <= w.last; ++s) {
                if (covered(s)) {
                    run.reset();
                    continue;
                }
                run = run ? Window{run->first, s, w.score} : Window{s, s, w.score};
                if (!longest || run->last - run->first > longest->last - longest->first) {
                    longest = run;
                }
            }
            if (longest) {
                kept.push_back(*longest);
            }
            if (kept.size() >= options.candidatesPerBeat) {
                break;
            }
        }

        // An outline bullet ("- pricing") may share no word with what was said: with a text encoder, the sentences
        // closest in meaning are candidates too (their embeddings are cached across beats).
        const std::optional<std::vector<float>>* beatVector = nullptr;
        if (inputs.embed && beat.outline) {
            beatVector = &embed(beat.text);
        }
        if (beatVector && *beatVector && kept.size() < options.candidatesPerBeat) {
            std::vector<std::pair<double, std::uint32_t>> nearest;
            for (std::uint32_t i = 0; i < sentences.size(); ++i) {
                const shorts::Sentence& s = corpus.sources[sentences[i].source].sentences[sentences[i].sentence];
                const std::optional<std::vector<float>>& v = embed(spanText(corpus, WordSpan{sentences[i].source, s.first, s.last}));
                if (v) {
                    nearest.emplace_back(dotf(**beatVector, *v), i);
                }
            }
            std::ranges::sort(nearest, [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
            for (const auto& [cosine, i] : nearest) {
                if (kept.size() >= options.candidatesPerBeat) {
                    break;
                }
                if (std::ranges::none_of(kept, [&](const Window& k) { return i >= k.first && i <= k.last; })) {
                    kept.push_back(Window{i, i, 0.0});
                }
            }
        }

        // 2. Alignment inside each window (a few words of slack each side).
        std::vector<Candidate>& candidates = perBeat[b];
        for (const Window& w : kept) {
            const IndexedSentence& firstSentence = sentences[w.first];
            const SpeechSource& source = corpus.sources[firstSentence.source];
            const shorts::Sentence& s0 = source.sentences[firstSentence.sentence];
            const shorts::Sentence& s1 = source.sentences[sentences[w.last].sentence];
            const std::size_t slack = 3;
            const std::size_t firstWord = s0.first > slack ? s0.first - slack : 0;
            const std::size_t lastWord = std::min(s1.last + slack, source.transcript.size() - 1);
            const std::size_t unitBegin = source.unitStart[firstWord];
            const std::size_t unitEnd = source.unitStart[lastWord + 1];
            const std::span<const Unit> target(source.units.data() + unitBegin, unitEnd - unitBegin);
            const Alignment a = align(query, target);
            Candidate candidate;
            double lexical = 0.0;
            WordSpan span;
            if (a.valid()) {
                lexical = a.quality();
                span = wordsOfUnits(source, firstSentence.source, unitBegin + a.begin, unitBegin + a.end);
            } else {
                span = WordSpan{firstSentence.source, s0.first, s1.last};
            }
            double match = lexical;
            bool byMeaning = false;
            if (beatVector && *beatVector) {
                // Outline beats are loose: the words of a sentence that means the bullet count too.
                if (!a.valid() || lexical < options.minMatch + 0.15) {
                    span = WordSpan{firstSentence.source, s0.first, s1.last};
                }
                const std::optional<std::vector<float>>& textVector = embed(spanText(corpus, span));
                if (textVector) {
                    const double meaning = meaningFromCosine(dotf(**beatVector, *textVector));
                    const double blended = (1.0 - options.embedWeight) * std::max(lexical, w.score) + options.embedWeight * meaning;
                    // A bullet said in other words: the meaning alone can carry it.
                    const double viaMeaning = std::max(blended, 0.9 * meaning);
                    if (viaMeaning > match) {
                        byMeaning = 0.9 * meaning >= blended || meaning * options.embedWeight > lexical * (1.0 - options.embedWeight);
                        match = viaMeaning;
                    }
                }
            } else if (beat.outline && !a.valid()) {
                continue;
            }
            // Strip fillers at the edges of the span.
            const SpeechSource& src = corpus.sources[span.source];
            while (span.first < span.last && src.filler[span.first]) {
                ++span.first;
            }
            while (span.last > span.first && src.filler[span.last]) {
                --span.last;
            }
            if (match < options.minMatch) {
                continue;
            }
            candidate.take.span = span;
            candidate.take.match = match;
            candidate.take.byMeaning = byMeaning;
            candidate.lexical = lexical;
            candidates.push_back(std::move(candidate));
            // Takes said one after another can share a window (or a transcript sentence): look for more in what is
            // left on either side of the one found.
            if (!a.valid()) {
                continue;
            }
            const std::size_t minUnits = std::max<std::size_t>(3, (query.size() + 1) / 2);
            std::vector<std::pair<std::size_t, std::size_t>> rest{{unitBegin, unitBegin + a.begin}, {unitBegin + a.end, unitEnd}};
            for (std::size_t more = 0; !rest.empty() && more < options.maxAlternates;) {
                const auto [lo, hi] = rest.back();
                rest.pop_back();
                if (hi <= lo || hi - lo < minUnits) {
                    continue;
                }
                const Alignment again = align(query, std::span<const Unit>(source.units.data() + lo, hi - lo));
                if (!again.valid() || again.quality() < options.minMatch) {
                    continue;
                }
                WordSpan other = wordsOfUnits(source, firstSentence.source, lo + again.begin, lo + again.end);
                while (other.first < other.last && src.filler[other.first]) {
                    ++other.first;
                }
                while (other.last > other.first && src.filler[other.last]) {
                    --other.last;
                }
                Candidate extra;
                extra.take.span = other;
                extra.take.match = again.quality();
                extra.lexical = again.quality();
                candidates.push_back(std::move(extra));
                ++more;
                rest.emplace_back(lo, lo + again.begin);
                rest.emplace_back(lo + again.end, hi);
            }
        }
        // Distinct takes only: the same words found from two windows count once.
        std::ranges::stable_sort(candidates, [](const Candidate& x, const Candidate& y) { return x.take.match > y.take.match; });
        std::vector<Candidate> distinct;
        for (Candidate& c : candidates) {
            const std::size_t length = c.take.span.last - c.take.span.first + 1;
            const bool duplicate = std::ranges::any_of(distinct, [&](const Candidate& d) {
                return overlapWords(c.take.span, d.take.span) * 2 > std::min(length, d.take.span.last - d.take.span.first + 1);
            });
            if (!duplicate) {
                distinct.push_back(std::move(c));
            }
        }
        candidates = std::move(distinct);
        if (!candidates.empty()) {
            const double bestMatch = candidates.front().take.match;
            std::erase_if(candidates, [&](const Candidate& c) { return c.take.match < bestMatch - options.alternateMargin; });
        }
    }

    // 3. Judge the takes: audio first in one pass per file, then the scores.
    if (inputs.audio) {
        std::vector<edit::AudioEnergyProbe::Range> ranges;
        for (const auto& candidates : perBeat) {
            for (const Candidate& c : candidates) {
                const auto r = audioRanges(corpus, c.take.span);
                ranges.insert(ranges.end(), r.begin(), r.end());
            }
        }
        inputs.audio->prefetch(ranges);
    }
    std::vector<double> levels;
    for (auto& candidates : perBeat) {
        for (Candidate& c : candidates) {
            c.take.quality = assessSpan(corpus, c.take.span, c.take.match, options.corpus);
            if (inputs.audio) {
                measureAudio(corpus, c.take.span, *inputs.audio, c.take.quality);
                if (c.take.quality.rmsDb && std::isfinite(*c.take.quality.rmsDb)) {
                    levels.push_back(*c.take.quality.rmsDb);
                }
            }
        }
    }
    std::optional<double> medianLevel;
    if (!levels.empty()) {
        std::ranges::sort(levels);
        medianLevel = levels[levels.size() / 2];
    }
    for (std::size_t b = 0; b < script.beats.size(); ++b) {
        BeatPlan beatPlan;
        beatPlan.beat = script.beats[b];
        std::vector<Candidate>& candidates = perBeat[b];
        for (Candidate& c : candidates) {
            scoreTake(c.take.quality, medianLevel, corpus.sources[c.take.span.source].wordsPerSecond);
            c.take.text = spanText(corpus, c.take.span);
            c.take.media = *mediaRangeWithHandles(corpus, c.take.span, options.handle);
        }
        std::ranges::stable_sort(candidates, [](const Candidate& x, const Candidate& y) {
            if (x.take.quality.score != y.take.quality.score) {
                return x.take.quality.score > y.take.quality.score;
            }
            return std::pair{x.take.span.source, x.take.span.first} < std::pair{y.take.span.source, y.take.span.first};
        });
        if (candidates.size() > options.maxAlternates + 1) {
            candidates.resize(options.maxAlternates + 1);
        }
        for (Candidate& c : candidates) {
            beatPlan.takes.push_back(std::move(c.take));
        }
        if (beatPlan.takes.empty()) {
            beatPlan.note = "not found in the media";
        } else {
            const std::size_t pick = preferredTake(beatPlan.takes, options.preference);
            std::rotate(beatPlan.takes.begin(), beatPlan.takes.begin() + static_cast<std::ptrdiff_t>(pick),
                        beatPlan.takes.begin() + static_cast<std::ptrdiff_t>(pick) + 1);
            const BeatTake& chosen = beatPlan.takes.front();
            double margin = 1.0;
            if (beatPlan.takes.size() > 1) {
                margin = std::clamp((chosen.quality.score - beatPlan.takes[1].quality.score) / 0.1, 0.0, 1.0);
            }
            beatPlan.confidence = std::clamp(0.8 * chosen.match + 0.2 * margin, 0.0, 1.0);
            if (chosen.byMeaning) {
                beatPlan.note = "matched by meaning";
            } else if (chosen.match < 0.7) {
                beatPlan.note = "partial match: the words differ from the script";
            }
        }
        plan.beats.push_back(std::move(beatPlan));
    }
    const std::size_t missing = script.beats.size() - plan.found();
    if (missing > 0) {
        plan.warnings.push_back(std::to_string(missing) + " of " + std::to_string(script.beats.size()) +
                                " beats were not found in the media");
    }
    plan.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    return plan;
}

std::expected<AssembledCut, Error> assembleRoughCut(const ProjectState& state, const RoughCutPlan& plan, const AssemblyOptions& options) {
    if (plan.found() == 0) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "no beat of the script was found in the media"));
    }
    wabcut::edit::SequenceSettings settings;
    settings.name = !options.name.empty() ? options.name
                    : plan.script.title.empty() ? std::string("Rough cut")
                                                : "Rough cut: " + truncateUtf8(plan.script.title, 80);
    settings.videoTracks = options.broll.empty() ? 1 : 2;
    settings.audioTracks = 1;
    const Sequence* like = options.settingsFrom ? state.findSequence(*options.settingsFrom) : nullptr;
    if (like) {
        settings.frameRate = like->frameRate;
        settings.width = like->width;
        settings.height = like->height;
        settings.audioRate = like->audioRate;
        settings.dropFrame = like->dropFrame;
    } else {
        for (const BeatPlan& beat : plan.beats) {
            if (beat.found()) {
                const MediaItem* media = state.findMedia(beat.takes.front().media.media);
                if (media && media->info.hasVideo()) {
                    const VideoStreamInfo& video = media->info.video.front();
                    if (video.frameRate.isValid() && video.frameRate.isExact()) {
                        settings.frameRate = video.frameRate.normalized();
                    }
                    if (video.width > 0 && video.height > 0) {
                        const bool turned = video.rotationDegrees == 90 || video.rotationDegrees == 270;
                        settings.width = turned ? video.height : video.width;
                        settings.height = turned ? video.width : video.height;
                    }
                }
                break;
            }
        }
    }
    auto created = wabcut::edit::addSequence(state, settings);
    if (!created) {
        return std::unexpected(created.error());
    }
    AssembledCut out;
    out.state = std::move(created->state);
    out.sequence = created->id;
    out.beatRanges.assign(plan.beats.size(), std::nullopt);
    const Sequence& seq = *out.state.findSequence(out.sequence);
    const TrackId v1 = seq.videoTracks.front()->id;
    const TrackId a1 = seq.audioTracks.front()->id;
    const std::optional<TrackId> v2 = seq.videoTracks.size() > 1 ? std::optional(seq.videoTracks[1]->id) : std::nullopt;
    const Ticks frame{settings.frameRate.ticksPerUnit()};
    Ticks cursor{};
    std::string section;
    std::vector<Marker> markers;
    for (std::size_t b = 0; b < plan.beats.size(); ++b) {
        const BeatPlan& beat = plan.beats[b];
        if (!beat.found()) {
            continue;
        }
        const MediaRange& take = beat.takes.front().media;
        const MediaItem* media = out.state.findMedia(take.media);
        if (!media) {
            out.warnings.push_back("beat " + std::to_string(b + 1) + ": its media is no longer in the project");
            continue;
        }
        // Whole frames of the sequence, within the media.
        Ticks length{(take.source.duration.value + frame.value - 1) / frame.value * frame.value};
        const std::optional<Ticks> extent = media->sourceExtent(StreamRef{StreamKind::Audio, take.audioStream});
        if (extent && take.source.start + length > *extent) {
            length = Ticks{(*extent - take.source.start).value / frame.value * frame.value};
        }
        if (length <= Ticks{}) {
            out.warnings.push_back("beat " + std::to_string(b + 1) + ": its take is shorter than a frame");
            continue;
        }
        wabcut::edit::SourceClip source;
        source.media = take.media;
        source.sourceRange = TimeRange{take.source.start, length};
        if (media->info.hasVideo()) {
            source.patches.push_back({StreamRef{StreamKind::Video, 0}, v1});
        }
        source.patches.push_back({StreamRef{StreamKind::Audio, take.audioStream}, a1});
        auto placed = wabcut::edit::insertEdit(out.state, out.sequence, source, cursor);
        if (!placed) {
            out.warnings.push_back("beat " + std::to_string(b + 1) + ": " + placed.error().message);
            continue;
        }
        out.state = std::move(placed->state);
        out.clips += placed->placed.size();
        out.beatRanges[b] = TimeRange{cursor, length};
        if (options.sectionMarkers && !beat.beat.section.empty() && beat.beat.section != section) {
            Marker m;
            m.start = cursor;
            m.name = truncateUtf8(beat.beat.section, 200);
            m.colorIndex = 3;
            markers.push_back(std::move(m));
            section = beat.beat.section;
        }
        cursor = cursor + length;
    }
    for (const BrollSuggestion& b : options.broll) {
        if (!v2 || b.beat >= out.beatRanges.size() || !out.beatRanges[b.beat]) {
            continue;
        }
        const MediaItem* media = out.state.findMedia(b.media);
        if (!media || !media->info.hasVideo()) {
            continue;
        }
        const TimeRange slot = *out.beatRanges[b.beat];
        Ticks length = std::min(slot.duration, Ticks{b.source.duration.value / frame.value * frame.value});
        length = std::max(length, std::min(slot.duration, Ticks::fromSeconds(1)));
        Ticks start = b.source.start;
        if (const auto extent = media->sourceExtent(StreamRef{StreamKind::Video, 0})) {
            if (start + length > *extent) {
                start = std::max(Ticks{}, *extent - length);
                length = std::min(length, *extent - start);
            }
        }
        wabcut::edit::SourceClip source;
        source.media = b.media;
        source.sourceRange = TimeRange{start, length};
        source.patches.push_back({StreamRef{StreamKind::Video, 0}, *v2});
        auto placed = wabcut::edit::overwriteEdit(out.state, out.sequence, source, slot.start);
        if (!placed) {
            out.warnings.push_back("B-roll for beat " + std::to_string(b.beat + 1) + ": " + placed.error().message);
            continue;
        }
        out.state = std::move(placed->state);
        ++out.brollPlaced;
    }
    auto next = std::make_shared<Sequence>(*out.state.findSequence(out.sequence));
    if (next->videoTracks.size() > 1) {
        auto broll = std::make_shared<Track>(*next->videoTracks[1]);
        broll->name = "B-roll (suggested)";
        next->videoTracks[1] = std::move(broll);
    }
    for (Marker& m : markers) {
        next->markers.push_back(std::move(m));
    }
    std::ranges::stable_sort(next->markers, [](const Marker& a, const Marker& b) { return a.start < b.start; });
    next->derivedFrom = SequenceDerivation{SequenceId{}, std::string(kRoughCutGenerator), recordToJson(recordOf(plan))};
    out.state = out.state.withSequence(std::move(next));
    return out;
}

// --- The record --------------------------------------------------------------------------------------------------

RoughCutRecord recordOf(const RoughCutPlan& plan) {
    const auto build = [&](std::size_t alternates, std::size_t textBytes) {
        RoughCutRecord record;
        record.title = truncateUtf8(plan.script.title, 200);
        for (const BeatPlan& beat : plan.beats) {
            RecordedBeat r;
            r.text = textBytes > 0 ? truncateUtf8(beat.beat.text, textBytes) : std::string();
            for (std::size_t t = 0; t < beat.takes.size() && t <= alternates; ++t) {
                r.takes.push_back({beat.takes[t].media, std::round(beat.takes[t].quality.score * 1000.0) / 1000.0});
            }
            record.beats.push_back(std::move(r));
        }
        return record;
    };
    for (const auto& [alternates, text] : {std::pair<std::size_t, std::size_t>{4, 120}, {3, 80}, {2, 60}, {1, 40}, {1, 0}, {0, 0}}) {
        RoughCutRecord record = build(alternates, text);
        if (recordToJson(record).size() + 64 <= SequenceDerivation::kMaxParamsBytes) {
            return record;
        }
    }
    return build(0, 0);
}

std::string recordToJson(const RoughCutRecord& record) {
    Json beats = Json::array();
    for (const RecordedBeat& beat : record.beats) {
        Json takes = Json::array();
        for (const RecordedTake& take : beat.takes) {
            takes.push_back(Json::array({take.media.media.value, take.media.audioStream, take.media.source.start.value,
                                         take.media.source.duration.value, take.score}));
        }
        beats.push_back(Json{{"text", beat.text}, {"chosen", beat.chosen}, {"takes", std::move(takes)}});
    }
    return Json{{"title", record.title}, {"beats", std::move(beats)}}.dump(-1, ' ', false, Json::error_handler_t::replace);
}

std::expected<RoughCutRecord, Error> recordFromJson(std::string_view text) {
    const Json json = Json::parse(text, nullptr, false);
    const auto bad = [](std::string what) { return std::unexpected(Error(ErrorCode::ParseError, "the rough-cut record is damaged: " + what)); };
    if (!json.is_object() || !json.contains("beats") || !json["beats"].is_array()) {
        return bad("no beats");
    }
    RoughCutRecord record;
    if (json.contains("title") && json["title"].is_string()) {
        record.title = json["title"].get<std::string>();
    }
    for (const Json& beat : json["beats"]) {
        if (!beat.is_object() || !beat.contains("takes") || !beat["takes"].is_array()) {
            return bad("a beat without takes");
        }
        RecordedBeat r;
        if (beat.contains("text") && beat["text"].is_string()) {
            r.text = beat["text"].get<std::string>();
        }
        for (const Json& take : beat["takes"]) {
            if (!take.is_array() || take.size() != 5 || !take[0].is_number_unsigned() || !take[1].is_number_integer() ||
                !take[2].is_number_integer() || !take[3].is_number_integer() || !take[4].is_number()) {
                return bad("a take is not [media, stream, start, length, score]");
            }
            RecordedTake t;
            t.media.media = MediaId{take[0].get<std::uint64_t>()};
            t.media.audioStream = take[1].get<int>();
            t.media.source = TimeRange{Ticks{take[2].get<std::int64_t>()}, Ticks{take[3].get<std::int64_t>()}};
            t.score = take[4].get<double>();
            if (t.media.audioStream < 0 || t.media.source.duration <= Ticks{} || t.media.source.start < Ticks{}) {
                return bad("a take has an impossible range");
            }
            r.takes.push_back(t);
        }
        if (beat.contains("chosen") && beat["chosen"].is_number_unsigned()) {
            r.chosen = beat["chosen"].get<std::size_t>();
        }
        if (!r.takes.empty() && r.chosen >= r.takes.size()) {
            return bad("a chosen take that does not exist");
        }
        record.beats.push_back(std::move(r));
    }
    return record;
}

std::expected<RoughCutRecord, Error> recordOfSequence(const Sequence& sequence) {
    if (!sequence.derivedFrom || sequence.derivedFrom->generator != kRoughCutGenerator) {
        return std::unexpected(Error(ErrorCode::InvalidArgument,
                                     "sequence '" + sequence.name + "' was not made by roughcut.fromScript, so it has no alternate takes"));
    }
    return recordFromJson(sequence.derivedFrom->params);
}

} // namespace wabcut::ai::roughcut
