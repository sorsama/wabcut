#pragma once

// The smart-search index: what was indexed of each media file, by source fingerprint, so every project that uses the
// file shares it and an edited file (a new fingerprint) is simply indexed anew.
//
//   index.sqlite   sources, keyframes (their vectors as float32 BLOBs) and transcript phrases, plus an FTS5 table of
//                  the phrases with the trigram tokenizer (TextQuery.h); SQLite is the source of truth
//   visual.hnsw    hnswlib's graph over the keyframe vectors (inner product = cosine on normalised vectors): a cache,
//                  rebuilt from SQLite when it is missing, damaged or out of date
//
// Vector search is exact (a scan of the allowed keyframes) up to `exactLimit` keyframes, and the HNSW graph (filtered to
// the allowed sources) beyond, where a scan would be slow. The index holds vectors of one model version; opening it
// with another drops its vectors (they are not comparable) and keeps the phrases. A damaged database is deleted and
// recreated: it is a cache, the AnalysisCache keeps the embeddings. Thread-safe (one internal lock).

#include <wabcut/ai/search/Embedding.h>
#include <wabcut/ai/search/TextQuery.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/Time.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

namespace wabcut::ai::search {

struct IndexedKeyframe {
    TimeRange range; ///< the stretch of the source it stands for
    Ticks time;      ///< the frame
    Vector vector;   ///< L2-normalised, the index's dimension
};

struct IndexedPhrase {
    TimeRange range;
    std::vector<std::string> words;
    std::vector<TimeRange> wordTimes; ///< one per word
};

struct IndexedSource {
    std::uint64_t fingerprint = 0;
    std::string name;          ///< file name when it was indexed (informational)
    std::string visualVersion; ///< model version of its keyframes (+ "|redacted:<signature>" when redacted ranges were
                               ///< left out); "" = no visual index
    std::string transcriptKey; ///< identity of the transcript its phrases came from; "" = none indexed
    std::size_t keyframes = 0;
    std::size_t phrases = 0;
};

struct VisualMatch {
    std::uint64_t fingerprint = 0;
    TimeRange range;
    Ticks time;
    float similarity = 0.0f; ///< cosine
};

struct TextMatch {
    std::uint64_t fingerprint = 0;
    TimeRange range;
    double relevance = 0.0; ///< higher is better (-bm25 for FTS matches)
    bool spaced = true;     ///< its language puts spaces between words (else they are joined without)
    std::vector<std::string> words;
    std::vector<TimeRange> wordTimes;
};

struct SearchIndexOptions {
    std::size_t exactLimit = 20'000; ///< allowed keyframes up to which vector search scans them all
    std::size_t hnswM = 16;
    std::size_t hnswEfConstruction = 200;
    std::size_t hnswEfSearch = 128;
};

class SearchIndex {
public:
    static constexpr int kSchemaVersion = 1;

    /// Opens (creating) the index in `dir` for vectors of `modelVersion` and `dimension`. Errors: IoError (the
    /// directory or database cannot be used), Unsupported (an SQLite without FTS5 or the trigram tokenizer).
    static std::expected<std::unique_ptr<SearchIndex>, Error> open(const std::filesystem::path& dir, std::string modelVersion,
                                                                    int dimension, SearchIndexOptions options = {});
    /// <user cache>/search/v1
    static std::filesystem::path defaultDir();

    ~SearchIndex();
    SearchIndex(const SearchIndex&) = delete;
    SearchIndex& operator=(const SearchIndex&) = delete;

    const std::string& modelVersion() const;
    int dimension() const;

    std::optional<IndexedSource> source(std::uint64_t fingerprint) const;
    std::vector<IndexedSource> sources() const;

    /// Replaces the source's keyframes (vectors of the index's dimension); `variant` is appended to the model version
    /// recorded for them (FootageSearch marks keyframes made without redacted ranges). Errors: InvalidArgument, IoError.
    std::expected<void, Error> putKeyframes(std::uint64_t fingerprint, const std::string& name,
                                            std::span<const IndexedKeyframe> keyframes, std::string_view variant = {});
    /// Replaces the source's transcript phrases; `language` decides whether they are also indexed without spaces.
    std::expected<void, Error> putPhrases(std::uint64_t fingerprint, const std::string& name, const std::string& transcriptKey,
                                          const std::string& language, std::span<const IndexedPhrase> phrases);
    std::expected<void, Error> removeSource(std::uint64_t fingerprint);
    /// Removes every source not in `keep`; returns how many went.
    std::expected<std::size_t, Error> prune(const std::unordered_set<std::uint64_t>& keep);

    /// The `k` keyframes of the allowed sources most similar to `query` (best first).
    std::expected<std::vector<VisualMatch>, Error> nearest(std::span<const float> query, std::size_t k,
                                                           const std::unordered_set<std::uint64_t>& allowed) const;
    /// The `k` phrases of the allowed sources that match `query` best (best first).
    std::expected<std::vector<TextMatch>, Error> matchText(const TextQuery& query, std::size_t k,
                                                           const std::unordered_set<std::uint64_t>& allowed) const;

    std::size_t keyframeCount() const;
    std::size_t phraseCount() const;
    /// Whether the last nearest() used the HNSW graph (else it scanned).
    bool usedGraph() const;

    /// Saves the HNSW graph if it changed (also on destruction). Errors: IoError.
    std::expected<void, Error> flush();

private:
    struct Impl;
    explicit SearchIndex(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

} // namespace wabcut::ai::search
