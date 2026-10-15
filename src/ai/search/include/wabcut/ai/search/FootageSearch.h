#pragma once

// Smart search over a project's footage (docs/ai.md, "Smart search"): find the moments that show or say what a query
// describes, as timecoded source ranges of the project's media.
//
// Indexing (index()) is a media-analysis job: per media file, shot-aware keyframes (C.5 shots, Keyframes.h) are
// decoded, embedded (Embedder, normally SigLIP 2 in wabcut-embed) and stored in the AnalysisCache by source fingerprint
// and model version, and the file's transcript is cut into phrases; both go into the SearchIndex. Everything is keyed
// by the file's fingerprint, so an unchanged file is indexed once for every project, and a changed file is indexed
// anew. Searching (search()) is a read: the query's text vector against the keyframes, its terms against the phrases
// (FTS5 trigram), fused by rank (Ranking.h), within the media the filters allow. Without an embedder (a build without
// ONNX, models not downloaded) transcripts are still indexed and searched, and results say visual search is off.
//
// Thread-safe: search() may run while index() works (the index locks; the embedder serialises its calls).

#include <wabcut/ai/Transcript.h>
#include <wabcut/ai/analysis/ShotDetection.h>
#include <wabcut/ai/search/Embedding.h>
#include <wabcut/ai/search/Keyframes.h>
#include <wabcut/ai/search/Ranking.h>
#include <wabcut/ai/search/SearchIndex.h>
#include <wabcut/model/ProjectState.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace wabcut::ai::search {

/// What indexing needs from the project's media files. MediaSearchAccess (full builds) decodes them; tests hand
/// over fixtures. Implementations must be thread-safe.
class SearchMediaAccess {
public:
    virtual ~SearchMediaAccess() = default;
    /// The file's source fingerprint (media::sourceFingerprint, the AnalysisCache key); nullopt when offline.
    virtual std::optional<std::uint64_t> fingerprint(const MediaItem& media) = 0;
    /// The file's date ("YYYY-MM-DD", local time of its last change) for date filters; "" when unknown.
    virtual std::string date(const MediaItem& media) = 0;
    /// The shots of its video (C.5, cached in the AnalysisCache).
    virtual std::expected<analysis::ShotAnalysis, Error> shots(const MediaItem& media, const analysis::AnalysisProgress& progress) = 0;
    /// Pictures of the frames that contain `times` (source time), `width` pixels wide.
    virtual std::expected<std::vector<Image>, Error> frames(const MediaItem& media, std::span<const Ticks> times, int width,
                                                            std::stop_token stop) = 0;
    /// The transcript of its first audio stream, or null.
    virtual std::shared_ptr<const Transcript> transcript(const MediaItem& media) = 0;
};

struct FootageSearchOptions {
    /// AnalysisCache root for the keyframe embeddings; empty = not cached (only the search index keeps them).
    std::filesystem::path cacheRoot;
    KeyframeOptions keyframes;
    int frameWidth = 384;            ///< keyframes are decoded this wide, then resized for the model
    std::size_t frameBatch = 32;     ///< keyframes decoded and embedded at a time (bounds memory)
    FusionOptions fusion;
    std::size_t candidates = 200;    ///< per list (visual, transcript) before fusion
    /// Visual matches below this cosine are dropped. SigLIP 2 base's own calibration (logit scale 112.7, bias -16.77):
    /// a match is 50 % likely at 0.149, 1 % at 0.108 and practically never near 0; 0.06 keeps weak but real matches
    /// for the ranking and drops what is unrelated.
    float minSimilarity = 0.06f;
};

struct SearchFilters {
    std::string name;                 ///< media name contains this (case-insensitive)
    std::string from;                 ///< media date on or after ("YYYY-MM-DD")
    std::string to;                   ///< media date on or before
    std::optional<Ticks> minDuration; ///< media length
    std::optional<Ticks> maxDuration;
    std::vector<MediaId> media;       ///< only these items (empty = all)
    bool visual = true;
    bool transcript = true;
};

struct SearchHit {
    MediaId media;
    std::string mediaName;
    std::uint64_t fingerprint = 0;
    TimeRange range;                   ///< source range (what dragging the hit inserts)
    Ticks thumbnail;                   ///< source time of the frame that matched
    double score = 0.0;
    std::optional<float> similarity;   ///< the frames matched (cosine)
    std::optional<double> relevance;   ///< the words matched
    std::string snippet;               ///< the matched phrase
    std::vector<std::string> matchedWords;
    std::vector<std::string> words;    ///< the phrase's words (the snippet, word by word)
    std::vector<bool> wordMatched;     ///< one per word: the query matched it
    bool spaced = true;                ///< the words are written with spaces between them
};

struct SearchResults {
    std::string query;
    std::vector<SearchHit> hits;
    std::size_t mediaSearched = 0;     ///< media in scope with an index entry
    std::vector<MediaId> notIndexed;   ///< media in scope without one (index them first)
    bool visual = false;               ///< the frames were searched
    std::vector<std::string> warnings;
    double milliseconds = 0.0;
};

struct IndexProgress {
    std::size_t index = 0; ///< media being indexed (0-based)
    std::size_t count = 0;
    std::string media;
    double fraction = 0.0; ///< of the whole job
};

struct IndexReport {
    std::size_t indexed = 0;   ///< media (re)indexed
    std::size_t upToDate = 0;
    std::size_t failed = 0;
    std::size_t keyframes = 0; ///< keyframes embedded or restored
    std::size_t phrases = 0;
    bool visual = false;       ///< frames were indexed
    std::vector<std::string> errors;
};

struct SearchStatus {
    std::size_t media = 0;     ///< media files of the project
    std::size_t indexed = 0;   ///< of them, with an index entry
    std::size_t visual = 0;    ///< of them, with keyframes of the current model
    std::size_t transcript = 0;
    bool visualAvailable = false;
};

class FootageSearch {
public:
    /// `embedder` may be null (visual search off).
    FootageSearch(FootageSearchOptions options, std::shared_ptr<SearchIndex> index, std::shared_ptr<SearchMediaAccess> access,
                  std::shared_ptr<Embedder> embedder);

    /// Media of `state` (`media`: these items; empty = all) brought up to date in the index. `progress` returning
    /// false cancels. Errors: Cancelled; per-media failures are reported in IndexReport::errors.
    std::expected<IndexReport, Error> index(const ProjectState& state, std::span<const MediaId> media,
                                            const std::function<bool(const IndexProgress&)>& progress = {}, std::stop_token stop = {});

    /// The best `limit` hits for `query`. Errors: InvalidArgument (empty query), Cancelled, IoError.
    std::expected<SearchResults, Error> search(const ProjectState& state, std::string_view query, const SearchFilters& filters,
                                               std::size_t limit, std::stop_token stop = {});

    SearchStatus status(const ProjectState& state);

    /// Replaces the embedder (models downloaded, settings changed); null turns visual search off.
    void setEmbedder(std::shared_ptr<Embedder> embedder);
    std::shared_ptr<Embedder> embedder() const;
    /// Why visual search is off, for messages ("" when it is on).
    void setVisualUnavailableReason(std::string reason);

    const std::shared_ptr<SearchIndex>& searchIndex() const { return m_index; }
    /// The media access (the app draws result thumbnails with it).
    const std::shared_ptr<SearchMediaAccess>& access() const { return m_access; }
    const FootageSearchOptions& options() const { return m_options; }

private:
    FootageSearchOptions m_options;
    std::shared_ptr<SearchIndex> m_index;
    std::shared_ptr<SearchMediaAccess> m_access;
    mutable std::mutex m_mutex; ///< m_embedder, m_unavailable
    std::shared_ptr<Embedder> m_embedder;
    std::string m_unavailable;
};

/// The phrases a transcript is searched by: its segments, cut at pauses of 1.5 s and after 24 words or 15 s.
std::vector<IndexedPhrase> phrasesOf(const Transcript& transcript);
/// Identity of a transcript's words (text and times) and language, for re-indexing only what changed.
std::string transcriptKey(const Transcript& transcript);

/// Payload of the keyframe embeddings in the AnalysisCache, and back.
std::string keyframesToJson(std::span<const IndexedKeyframe> keyframes, int dimension);
std::optional<std::vector<IndexedKeyframe>> keyframesFromJson(std::string_view json, int dimension);

/// The AnalysisCache analyser id of the keyframe embeddings.
inline constexpr std::string_view kKeyframeAnalyzer = "search-keyframes";

} // namespace wabcut::ai::search
