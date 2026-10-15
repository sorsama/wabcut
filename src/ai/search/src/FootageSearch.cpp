#include <wabcut/ai/search/FootageSearch.h>

#include <wabcut/ai/AnalysisCache.h>
#include <wabcut/ai/JsonAccess.h>
#include <wabcut/ai/Sha256.h>
#include <wabcut/ai/search/Tokenizer.h>
#include <wabcut/model/RedactionMedia.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <unordered_map>

namespace wabcut::ai::search {

namespace {

using Json = nlohmann::json;

constexpr Ticks kPhrasePause = Ticks{kTicksPerSecond * 3 / 2};
constexpr std::size_t kPhraseWords = 24;
constexpr Ticks kPhraseLength = Ticks{kTicksPerSecond * 15};
/// What a keyframe of a still picture stands for.
constexpr Ticks kStillRange = Ticks{kTicksPerSecond * 5};

std::string joined(std::span<const std::string> words, bool spaced) {
    std::string out;
    for (std::size_t i = 0; i < words.size(); ++i) {
        if (spaced && i > 0) {
            out += ' ';
        }
        out += words[i];
    }
    return out;
}

bool containsFolded(std::string_view text, std::string_view part) {
    return lowercaseUtf8(text).find(lowercaseUtf8(part)) != std::string::npos;
}

int videoStreamIndex(const MediaItem& media) { return media.info.video.empty() ? 0 : media.info.video.front().index; }

/// Where the media's enabled redactions hide its picture (media time), sorted; and a short signature of them.
/// Keyframes there are never decoded, embedded, cached or indexed (review wave 4, P2-9).
std::vector<TimeRange> redactedRanges(const ProjectState& state, MediaId media) {
    std::vector<TimeRange> out;
    for (const Redaction* r : redactionsCovering(state, media)) { // a subclip's master's too (review wave 5)
        if (r->enabled && r->source.duration > Ticks{}) {
            out.push_back(r->source);
        }
    }
    std::ranges::sort(out, [](const TimeRange& a, const TimeRange& b) { return std::pair{a.start, a.duration} < std::pair{b.start, b.duration}; });
    return out;
}

std::string redactionVariant(const std::vector<TimeRange>& ranges) {
    if (ranges.empty()) {
        return {};
    }
    std::string text;
    for (const TimeRange& r : ranges) {
        text += std::to_string(r.start.value) + ":" + std::to_string(r.duration.value) + ";";
    }
    return "|redacted:" + Sha256::hex(text).substr(0, 16);
}

/// Keyframes of the index's model, with or without redacted ranges left out.
bool currentModel(const std::string& visualVersion, const std::string& modelVersion) {
    return visualVersion == modelVersion || visualVersion.starts_with(modelVersion + "|redacted:");
}

std::string keyframeParams(const FootageSearchOptions& options) {
    return hashParams({{"interval", std::to_string(options.keyframes.interval.value)},
                       {"maxKeyframes", std::to_string(options.keyframes.maxKeyframes)},
                       {"frameWidth", std::to_string(options.frameWidth)},
                       {"shots", std::string(analysis::ShotAnalysis::kAnalyzer) + "-" + std::to_string(analysis::ShotAnalysis::kVersion)}});
}

} // namespace

// --- Phrases and payloads --------------------------------------------------------------------------------------------

std::vector<IndexedPhrase> phrasesOf(const Transcript& transcript) {
    std::vector<IndexedPhrase> out;
    std::vector<std::pair<std::size_t, std::size_t>> segments; // [first, end) word ranges
    for (const TranscriptSegment& s : transcript.segments) {
        const std::size_t first = std::min(s.firstWord, transcript.words.size());
        segments.emplace_back(first, std::min(first + s.wordCount, transcript.words.size()));
    }
    if (segments.empty()) {
        segments.emplace_back(0, transcript.words.size());
    }
    for (const auto& [first, end] : segments) {
        IndexedPhrase phrase;
        const auto close = [&] {
            if (!phrase.words.empty()) {
                phrase.range = TimeRange::fromStartEnd(phrase.wordTimes.front().start, phrase.wordTimes.back().end());
                out.push_back(std::move(phrase));
            }
            phrase = IndexedPhrase{};
        };
        for (std::size_t i = first; i < end; ++i) {
            const TranscriptWord& w = transcript.words[i];
            if (w.text.empty()) {
                continue;
            }
            if (!phrase.words.empty() &&
                (w.t0 - phrase.wordTimes.back().end() >= kPhrasePause || phrase.words.size() >= kPhraseWords ||
                 w.t1 - phrase.wordTimes.front().start >= kPhraseLength)) {
                close();
            }
            phrase.words.push_back(w.text);
            phrase.wordTimes.push_back(TimeRange::fromStartEnd(w.t0, std::max(w.t1, w.t0)));
        }
        close();
    }
    return out;
}

std::string transcriptKey(const Transcript& transcript) {
    Sha256 h;
    h.update(transcript.language);
    h.update(std::string_view("\n", 1));
    for (const TranscriptWord& w : transcript.words) {
        h.update(w.text);
        const std::string times = "\x1F" + std::to_string(w.t0.value) + "\x1F" + std::to_string(w.t1.value) + "\n";
        h.update(times);
    }
    return h.finishHex().substr(0, 32);
}

std::string keyframesToJson(std::span<const IndexedKeyframe> keyframes, int dimension) {
    Json list = Json::array();
    for (const IndexedKeyframe& k : keyframes) {
        list.push_back(Json{{"t0", k.range.start.value}, {"t1", k.range.end().value}, {"time", k.time.value}, {"v", encodeVector(k.vector)}});
    }
    return Json{{"dimension", dimension}, {"keyframes", std::move(list)}}.dump();
}

std::optional<std::vector<IndexedKeyframe>> keyframesFromJson(std::string_view json, int dimension) {
    const Json j = Json::parse(json, nullptr, false);
    if (j.is_discarded() || jsonIntOr(j, "dimension", 0) != dimension) {
        return std::nullopt;
    }
    const Json* list = jsonArray(j, "keyframes");
    if (!list) {
        return std::nullopt;
    }
    std::vector<IndexedKeyframe> out;
    for (const Json& k : *list) {
        const auto t0 = jsonInt(k, "t0");
        const auto t1 = jsonInt(k, "t1");
        const auto time = jsonInt(k, "time");
        const auto v = jsonString(k, "v");
        auto vector = v ? decodeVector(*v, static_cast<std::size_t>(dimension)) : std::nullopt;
        if (!t0 || !t1 || !time || !vector || *t1 < *t0) {
            return std::nullopt;
        }
        out.push_back(IndexedKeyframe{TimeRange::fromStartEnd(Ticks{*t0}, Ticks{*t1}), Ticks{*time}, std::move(*vector)});
    }
    return out;
}

// --- FootageSearch ---------------------------------------------------------------------------------------------------

FootageSearch::FootageSearch(FootageSearchOptions options, std::shared_ptr<SearchIndex> index,
                             std::shared_ptr<SearchMediaAccess> access, std::shared_ptr<Embedder> embedder)
    : m_options(std::move(options)), m_index(std::move(index)), m_access(std::move(access)), m_embedder(std::move(embedder)) {
    m_options.frameBatch = std::max<std::size_t>(m_options.frameBatch, 1);
    m_options.frameWidth = std::clamp(m_options.frameWidth, 16, 1024);
}

void FootageSearch::setEmbedder(std::shared_ptr<Embedder> embedder) {
    std::lock_guard lock(m_mutex);
    m_embedder = std::move(embedder);
}

std::shared_ptr<Embedder> FootageSearch::embedder() const {
    std::lock_guard lock(m_mutex);
    return m_embedder;
}

void FootageSearch::setVisualUnavailableReason(std::string reason) {
    std::lock_guard lock(m_mutex);
    m_unavailable = std::move(reason);
}

std::expected<IndexReport, Error> FootageSearch::index(const ProjectState& state, std::span<const MediaId> media,
                                                       const std::function<bool(const IndexProgress&)>& progress,
                                                       std::stop_token stop) {
    IndexReport report;
    const std::shared_ptr<Embedder> embedder = this->embedder();
    report.visual = embedder != nullptr && embedder->modelVersion() == m_index->modelVersion();

    std::vector<const MediaItem*> items;
    for (const MediaPtr& m : state.media) {
        if (m && !m->isGenerator() && (media.empty() || std::ranges::find(media, m->id) != media.end())) {
            items.push_back(m.get());
        }
    }
    bool cancelled = false;
    const auto report01 = [&](std::size_t i, const MediaItem& item, double within) {
        if (stop.stop_requested()) {
            cancelled = true;
        }
        if (progress && !cancelled) {
            const double fraction = (static_cast<double>(i) + std::clamp(within, 0.0, 1.0)) / static_cast<double>(std::max<std::size_t>(items.size(), 1));
            cancelled = !progress(IndexProgress{i, items.size(), item.name, fraction});
        }
        return !cancelled;
    };
    const auto cancelledError = [] { return std::unexpected(Error(ErrorCode::Cancelled, "indexing cancelled")); };

    for (std::size_t i = 0; i < items.size(); ++i) {
        const MediaItem& item = *items[i];
        if (!report01(i, item, 0.0)) {
            return cancelledError();
        }
        const auto fingerprint = m_access->fingerprint(item);
        if (!fingerprint) {
            ++report.failed;
            report.errors.push_back("'" + item.name + "': the file is offline");
            continue;
        }
        const auto entry = m_index->source(*fingerprint);
        bool changed = false;
        bool failed = false;

        // Frames: keyframes of the current model, from the analysis cache or embedded now.
        const bool wantsVisual = report.visual && item.info.hasVideo();
        const std::vector<TimeRange> redacted = redactedRanges(state, item.id);
        const std::string variant = redactionVariant(redacted);
        if (wantsVisual && !(entry && entry->visualVersion == m_index->modelVersion() + variant)) {
            const AnalysisKey key{*fingerprint, videoStreamIndex(item), std::string(kKeyframeAnalyzer), embedder->modelVersion(),
                                  keyframeParams(m_options) + variant};
            if (!variant.empty() && !m_options.cacheRoot.empty()) {
                // Embeddings of the whole picture, made before a redaction hid part of it: gone (P2-9).
                (void)AnalysisCache(m_options.cacheRoot)
                    .remove(AnalysisKey{*fingerprint, videoStreamIndex(item), std::string(kKeyframeAnalyzer), embedder->modelVersion(),
                                        keyframeParams(m_options)});
            }
            std::optional<std::vector<IndexedKeyframe>> keyframes;
            if (!m_options.cacheRoot.empty()) {
                if (auto cached = AnalysisCache(m_options.cacheRoot).get(key); cached && *cached) {
                    keyframes = keyframesFromJson(**cached, embedder->dimension());
                }
            }
            if (!keyframes) {
                std::vector<Keyframe> plan;
                if (item.info.isStillImage) {
                    plan.push_back(Keyframe{Ticks{0}, TimeRange{Ticks{0}, kStillRange}, TimeRange{Ticks{0}, kStillRange}});
                } else {
                    auto shots = m_access->shots(item, [&](double f) { return report01(i, item, 0.3 * f); });
                    if (!shots) {
                        if (cancelled || shots.error().code == ErrorCode::Cancelled) {
                            return cancelledError();
                        }
                        report.errors.push_back("'" + item.name + "': shots: " + shots.error().message);
                        failed = true;
                    } else {
                        plan = planKeyframes(*shots, m_options.keyframes);
                    }
                }
                std::erase_if(plan, [&](const Keyframe& k) {
                    return std::ranges::any_of(redacted, [&](const TimeRange& r) { return r.overlaps(k.range) || r.contains(k.time); });
                });
                std::vector<IndexedKeyframe> made;
                for (std::size_t first = 0; !failed && first < plan.size(); first += m_options.frameBatch) {
                    const std::size_t count = std::min(m_options.frameBatch, plan.size() - first);
                    std::vector<Ticks> times;
                    for (std::size_t k = first; k < first + count; ++k) {
                        times.push_back(plan[k].time);
                    }
                    auto frames = m_access->frames(item, times, m_options.frameWidth, stop);
                    if (frames && frames->size() != count) {
                        frames = std::unexpected(Error(ErrorCode::DecoderError, "fewer frames than asked"));
                    }
                    auto vectors = frames ? embedder->embedImages(*frames, stop)
                                          : std::expected<std::vector<Vector>, Error>(std::unexpected(frames.error()));
                    if (!vectors) {
                        if (stop.stop_requested() || vectors.error().code == ErrorCode::Cancelled) {
                            return cancelledError();
                        }
                        report.errors.push_back("'" + item.name + "': frames: " + vectors.error().message);
                        failed = true;
                        break;
                    }
                    for (std::size_t k = 0; k < count; ++k) {
                        made.push_back(IndexedKeyframe{plan[first + k].range, plan[first + k].time, std::move((*vectors)[k])});
                    }
                    if (!report01(i, item, 0.3 + 0.7 * static_cast<double>(first + count) / static_cast<double>(plan.size()))) {
                        return cancelledError();
                    }
                }
                if (!failed) {
                    keyframes = std::move(made);
                    if (!m_options.cacheRoot.empty()) {
                        (void)AnalysisCache(m_options.cacheRoot).put(key, keyframesToJson(*keyframes, embedder->dimension()));
                    }
                }
            }
            if (keyframes) {
                if (auto put = m_index->putKeyframes(*fingerprint, item.name, *keyframes, variant); !put) {
                    report.errors.push_back("'" + item.name + "': " + put.error().message);
                    failed = true;
                } else {
                    report.keyframes += keyframes->size();
                    changed = true;
                }
            }
        }

        // Words: the transcript's phrases, when the transcript changed.
        const std::shared_ptr<const Transcript> transcript = m_access->transcript(item);
        const std::string key = transcript ? transcriptKey(*transcript) : std::string();
        const bool hasPhrases = entry && !entry->transcriptKey.empty();
        if (transcript ? !(entry && entry->transcriptKey == key) : hasPhrases) {
            const auto phrases = transcript ? phrasesOf(*transcript) : std::vector<IndexedPhrase>{};
            if (auto put = m_index->putPhrases(*fingerprint, item.name, key, transcript ? transcript->language : std::string(), phrases);
                !put) {
                report.errors.push_back("'" + item.name + "': " + put.error().message);
                failed = true;
            } else {
                report.phrases += phrases.size();
                changed = true;
            }
        }
        if (!entry && !changed && !failed) {
            // Nothing to index (no video for this model, no transcript): recorded, so it counts as indexed.
            (void)m_index->putPhrases(*fingerprint, item.name, std::string(), std::string(), {});
            changed = true;
        }
        if (failed) {
            ++report.failed;
        } else if (changed) {
            ++report.indexed;
        } else {
            ++report.upToDate;
        }
        if (!report01(i, item, 1.0)) {
            return cancelledError();
        }
    }
    (void)m_index->flush();
    return report;
}

std::expected<SearchResults, Error> FootageSearch::search(const ProjectState& state, std::string_view query,
                                                          const SearchFilters& filters, std::size_t limit, std::stop_token stop) {
    const auto started = std::chrono::steady_clock::now();
    SearchResults results;
    results.query = std::string(query);
    if (query.find_first_not_of(" \t\r\n") == std::string_view::npos) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "smart search: the query is empty"));
    }
    const std::shared_ptr<Embedder> embedder = this->embedder();
    const bool modelMatches = embedder != nullptr && embedder->modelVersion() == m_index->modelVersion();

    std::unordered_set<std::uint64_t> allowed;
    std::unordered_map<std::uint64_t, const MediaItem*> mediaOf;
    bool anyKeyframes = false;
    for (const MediaPtr& m : state.media) {
        if (!m || m->isGenerator()) {
            continue;
        }
        const MediaItem& item = *m;
        if (!filters.media.empty() && std::ranges::find(filters.media, item.id) == filters.media.end()) {
            continue;
        }
        if (!filters.name.empty() && !containsFolded(item.name, filters.name)) {
            continue;
        }
        if ((filters.minDuration && item.info.duration < *filters.minDuration) ||
            (filters.maxDuration && item.info.duration > *filters.maxDuration)) {
            continue;
        }
        if (!filters.from.empty() || !filters.to.empty()) {
            const std::string date = m_access->date(item).substr(0, 10);
            if (date.empty() || (!filters.from.empty() && date < filters.from) || (!filters.to.empty() && date > filters.to)) {
                continue;
            }
        }
        const auto fingerprint = m_access->fingerprint(item);
        if (!fingerprint) {
            continue; // offline
        }
        const auto entry = m_index->source(*fingerprint);
        if (!entry) {
            results.notIndexed.push_back(item.id);
            continue;
        }
        allowed.insert(*fingerprint);
        mediaOf.try_emplace(*fingerprint, &item);
        anyKeyframes = anyKeyframes || (entry->keyframes > 0 && currentModel(entry->visualVersion, m_index->modelVersion()));
    }
    results.mediaSearched = allowed.size();
    if (!results.notIndexed.empty()) {
        results.warnings.push_back(std::to_string(results.notIndexed.size()) +
                                   " media item(s) are not indexed yet (search.indexMedia indexes them)");
    }

    const TextQuery textQuery = buildTextQuery(query);
    std::vector<VisualMatch> visual;
    if (filters.visual && !allowed.empty()) {
        if (!modelMatches) {
            std::lock_guard lock(m_mutex);
            results.warnings.push_back("visual search is off" + (m_unavailable.empty() ? std::string() : ": " + m_unavailable));
        } else if (anyKeyframes) {
            auto vector = embedder->embedText(query, stop);
            if (!vector) {
                if (vector.error().code == ErrorCode::Cancelled) {
                    return std::unexpected(vector.error());
                }
                results.warnings.push_back("visual search failed: " + vector.error().message);
            } else {
                auto found = m_index->nearest(*vector, m_options.candidates, allowed);
                if (!found) {
                    return std::unexpected(found.error());
                }
                for (VisualMatch& match : *found) {
                    // Keyframes indexed before a redaction hid that part of the picture never come back as hits
                    // (review wave 4, P2-9); the next indexing drops them.
                    const auto media = mediaOf.find(match.fingerprint);
                    const bool hidden = media != mediaOf.end() && std::ranges::any_of(redactedRanges(state, media->second->id), [&](const TimeRange& r) {
                        return r.overlaps(match.range) || r.contains(match.time);
                    });
                    if (match.similarity >= m_options.minSimilarity && !hidden) {
                        visual.push_back(match);
                    }
                }
                results.visual = true;
            }
        }
    }
    std::vector<TextMatch> text;
    if (filters.transcript && !textQuery.empty() && !allowed.empty()) {
        auto found = m_index->matchText(textQuery, m_options.candidates, allowed);
        if (!found) {
            return std::unexpected(found.error());
        }
        text = std::move(*found);
    }
    if (stop.stop_requested()) {
        return std::unexpected(Error(ErrorCode::Cancelled, "search cancelled"));
    }

    for (FusedHit& fused : fuse(visual, text, textQuery, m_options.fusion)) {
        if (results.hits.size() >= limit) {
            break;
        }
        const auto media = mediaOf.find(fused.fingerprint);
        if (media == mediaOf.end()) {
            continue;
        }
        SearchHit hit;
        hit.media = media->second->id;
        hit.mediaName = media->second->name;
        hit.fingerprint = fused.fingerprint;
        hit.range = fused.range;
        hit.thumbnail = fused.thumbnail;
        hit.score = fused.score;
        hit.similarity = fused.similarity;
        hit.relevance = fused.relevance;
        hit.snippet = joined(fused.words, fused.spaced);
        hit.words = fused.words;
        hit.wordMatched.assign(fused.words.size(), false);
        hit.spaced = fused.spaced;
        for (const std::size_t w : fused.matched) {
            if (w < fused.words.size()) {
                hit.matchedWords.push_back(fused.words[w]);
                hit.wordMatched[w] = true;
            }
        }
        results.hits.push_back(std::move(hit));
    }
    results.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    return results;
}

SearchStatus FootageSearch::status(const ProjectState& state) {
    SearchStatus status;
    const std::shared_ptr<Embedder> embedder = this->embedder();
    status.visualAvailable = embedder != nullptr && embedder->modelVersion() == m_index->modelVersion();
    for (const MediaPtr& m : state.media) {
        if (!m || m->isGenerator()) {
            continue;
        }
        ++status.media;
        const auto fingerprint = m_access->fingerprint(*m);
        const auto entry = fingerprint ? m_index->source(*fingerprint) : std::nullopt;
        if (!entry) {
            continue;
        }
        const bool visualDone = !m->info.hasVideo() || !status.visualAvailable ||
                                entry->visualVersion == m_index->modelVersion() + redactionVariant(redactedRanges(state, m->id));
        status.indexed += visualDone ? 1 : 0;
        status.visual += currentModel(entry->visualVersion, m_index->modelVersion()) && entry->keyframes > 0 ? 1 : 0;
        status.transcript += entry->phrases > 0 ? 1 : 0;
    }
    return status;
}

} // namespace wabcut::ai::search
