// Smart search (AI-8): the search index (SQLite FTS5 trigram + hnswlib) and the hybrid ranking, on fixture vectors.
// Persistence across reopening, invalidation by fingerprint and model version, the exact scan against the HNSW graph,
// damaged files, transcript search in English and Thai, and the latency of a search over 1,000 indexed shots.

#include "SearchTestUtil.h"

#include <wabcut/ai/Sha256.h>
#include <wabcut/ai/search/Ranking.h>
#include <wabcut/ai/search/SearchIndex.h>

#include <sqlite3.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <random>
#include <unordered_set>

using namespace wabcut;
using namespace wabcut::ai::search;
using namespace wabcut::ai::search::test;

namespace {

constexpr std::uint64_t kA = 0xA000000000000001ULL; // high bit set: stored as a negative SQLite integer
constexpr std::uint64_t kB = 0x0B00000000000002ULL;

IndexedKeyframe keyframe(double from, double to, Vector v) {
    return IndexedKeyframe{TimeRange::fromStartEnd(seconds(from), seconds(to)), seconds((from + to) / 2), std::move(v)};
}

IndexedPhrase phrase(std::vector<std::string> words, double from, double step = 0.5) {
    IndexedPhrase p;
    for (std::size_t i = 0; i < words.size(); ++i) {
        p.wordTimes.push_back(TimeRange::fromStartEnd(seconds(from + step * static_cast<double>(i)),
                                                      seconds(from + step * static_cast<double>(i + 1))));
    }
    p.words = std::move(words);
    p.range = TimeRange::fromStartEnd(p.wordTimes.front().start, p.wordTimes.back().end());
    return p;
}

std::unique_ptr<SearchIndex> openIndex(const std::filesystem::path& dir, std::string model = "fixture/1", int dim = kFixtureDim,
                                       SearchIndexOptions options = {}) {
    auto index = SearchIndex::open(dir, std::move(model), dim, options);
    REQUIRE(index);
    return std::move(*index);
}

const std::unordered_set<std::uint64_t> kBoth{kA, kB};

Vector randomUnit(std::mt19937& rng, int dim) {
    std::normal_distribution<float> n;
    Vector v(static_cast<std::size_t>(dim));
    for (float& x : v) {
        x = n(rng);
    }
    normalize(v);
    return v;
}

std::string wordsOf(const TextMatch& m) {
    std::string out;
    for (const auto& w : m.words) {
        out += (out.empty() ? "" : " ") + w;
    }
    return out;
}

} // namespace

TEST_CASE("The search index keeps keyframes and phrases across reopening", "[ai][search][index]") {
    TempDir dir("persist");
    {
        auto index = openIndex(dir.path);
        REQUIRE(index->putKeyframes(kA, "Beach.mp4",
                                    std::vector<IndexedKeyframe>{keyframe(0, 5, unit({1, 0, 0})), keyframe(5, 10, unit({0, 1, 0})),
                                                                 keyframe(10, 20, unit({0, 0, 1}))}));
        REQUIRE(index->putPhrases(kA, "Beach.mp4", "t1", "en",
                                  std::vector<IndexedPhrase>{phrase({"we", "walked", "along", "the", "beach"}, 6)}));
        REQUIRE(index->putKeyframes(kB, "Studio.mov", std::vector<IndexedKeyframe>{keyframe(0, 4, unit({0.9f, 0.1f, 0}))}));
        CHECK(index->keyframeCount() == 4);
        CHECK(index->phraseCount() == 1);
    }
    auto index = openIndex(dir.path);
    CHECK(index->keyframeCount() == 4);
    const auto source = index->source(kA);
    REQUIRE(source);
    CHECK(source->name == "Beach.mp4");
    CHECK(source->visualVersion == "fixture/1");
    CHECK(source->transcriptKey == "t1");
    CHECK(source->keyframes == 3);
    CHECK(source->phrases == 1);
    CHECK(index->sources().size() == 2);

    auto red = index->nearest(unit({1, 0, 0}), 3, kBoth);
    REQUIRE(red);
    REQUIRE(red->size() == 3);
    CHECK((*red)[0].fingerprint == kA);
    CHECK((*red)[0].range == TimeRange::fromStartEnd(seconds(0), seconds(5)));
    CHECK((*red)[0].similarity == Catch::Approx(1.0f));
    CHECK((*red)[1].fingerprint == kB); // 0.9 red
    CHECK((*red)[0].similarity >= (*red)[1].similarity);

    // Only the allowed sources are searched.
    auto onlyB = index->nearest(unit({0, 0, 1}), 3, {kB});
    REQUIRE(onlyB);
    REQUIRE(onlyB->size() == 1);
    CHECK((*onlyB)[0].fingerprint == kB);
    CHECK(index->nearest(unit({0, 0, 1}), 3, {})->empty());
    CHECK(index->nearest(Vector{1, 0}, 3, kBoth).error().code == ErrorCode::InvalidArgument);

    auto beach = index->matchText(buildTextQuery("BEACH"), 5, kBoth);
    REQUIRE(beach);
    REQUIRE(beach->size() == 1);
    CHECK(wordsOf((*beach)[0]) == "we walked along the beach");
    CHECK((*beach)[0].range.start == seconds(6));
    CHECK((*beach)[0].wordTimes.size() == 5);
    CHECK((*beach)[0].spaced);
}

TEST_CASE("Re-indexing a source replaces it; other fingerprints and other models invalidate", "[ai][search][index]") {
    TempDir dir("invalidate");
    {
        auto index = openIndex(dir.path);
        REQUIRE(index->putKeyframes(kA, "A", std::vector<IndexedKeyframe>{keyframe(0, 5, unit({1, 0, 0})), keyframe(5, 9, unit({0, 1, 0}))}));
        REQUIRE(index->putPhrases(kA, "A", "t1", "en", std::vector<IndexedPhrase>{phrase({"old", "words"}, 0)}));
        REQUIRE(index->putKeyframes(kB, "B", std::vector<IndexedKeyframe>{keyframe(0, 5, unit({0, 0, 1}))}));
        REQUIRE(index->putPhrases(kB, "B", "t2", "en", std::vector<IndexedPhrase>{phrase({"bravo", "words"}, 0)}));

        // Replacing A's keyframes and phrases leaves no trace of the old ones.
        REQUIRE(index->putKeyframes(kA, "A", std::vector<IndexedKeyframe>{keyframe(0, 9, unit({0, 1, 0}))}));
        REQUIRE(index->putPhrases(kA, "A", "t3", "en", std::vector<IndexedPhrase>{phrase({"new", "words"}, 0)}));
        CHECK(index->keyframeCount() == 2);
        CHECK(index->matchText(buildTextQuery("old"), 5, kBoth)->empty());
        CHECK(index->matchText(buildTextQuery("new"), 5, kBoth)->size() == 1);
        CHECK(index->matchText(buildTextQuery("words"), 5, kBoth)->size() == 2);
        CHECK(index->source(kA)->transcriptKey == "t3");

        // A file that changed has a new fingerprint: the old one is pruned once no project uses it.
        auto pruned = index->prune({kB});
        REQUIRE(pruned);
        CHECK(*pruned == 1);
        CHECK_FALSE(index->source(kA));
        CHECK(index->keyframeCount() == 1);
        CHECK(index->matchText(buildTextQuery("new"), 5, kBoth)->empty());
        CHECK(index->nearest(unit({0, 1, 0}), 5, kBoth)->size() == 1);
        REQUIRE(index->removeSource(kB));
        CHECK(index->sources().empty());
        REQUIRE(index->putKeyframes(kB, "B", std::vector<IndexedKeyframe>{keyframe(0, 5, unit({0, 0, 1}))}));
        REQUIRE(index->putPhrases(kB, "B", "t2", "en", std::vector<IndexedPhrase>{phrase({"bravo"}, 0)}));
    }
    // Another model's vectors are not comparable: they go, the phrases stay.
    auto other = openIndex(dir.path, "fixture/2");
    CHECK(other->keyframeCount() == 0);
    REQUIRE(other->source(kB));
    CHECK(other->source(kB)->visualVersion.empty());
    CHECK(other->matchText(buildTextQuery("bravo"), 5, kBoth)->size() == 1);
    CHECK(other->nearest(unit({0, 0, 1}), 5, kBoth)->empty());
    CHECK(other->putKeyframes(kB, "B", std::vector<IndexedKeyframe>{keyframe(0, 5, Vector{1, 0})}).error().code ==
          ErrorCode::InvalidArgument); // wrong dimension
}

TEST_CASE("Transcript search finds Thai without spaces, short words by LIKE, and ranks fuller matches first",
          "[ai][search][index][fts]") {
    TempDir dir("fts");
    auto index = openIndex(dir.path);
    REQUIRE(index->putPhrases(kA, "Thai.mp4", "th1", "th",
                              std::vector<IndexedPhrase>{phrase({"วันนี้", "แมว", "วิ่ง", "บน", "ชายหาด"}, 0),
                                                         phrase({"ขับ", "รถ", "ไป", "ตลาด"}, 10)}));
    REQUIRE(index->putPhrases(kB, "Meeting.mp4", "en1", "en",
                              std::vector<IndexedPhrase>{phrase({"the", "budget", "is", "fine"}, 0),
                                                         phrase({"the", "budget", "meeting", "moved", "to", "Friday"}, 5),
                                                         phrase({"the", "cat", "ran", "home"}, 12)}));

    // Thai: a run across words (indexed without spaces), a single word, and a 2-letter word through LIKE.
    auto run = index->matchText(buildTextQuery("แมววิ่ง"), 5, kBoth);
    REQUIRE(run);
    REQUIRE(run->size() == 1);
    CHECK((*run)[0].fingerprint == kA);
    CHECK_FALSE((*run)[0].spaced);
    CHECK(index->matchText(buildTextQuery("ชายหาด"), 5, kBoth)->size() == 1);
    auto car = index->matchText(buildTextQuery("รถ"), 5, kBoth);
    REQUIRE(car);
    REQUIRE(car->size() == 1);
    CHECK((*car)[0].range.start == seconds(10));

    // English: both terms beat one term; case does not matter; no match across words of a spaced language.
    auto budget = index->matchText(buildTextQuery("Budget MEETING"), 5, kBoth);
    REQUIRE(budget);
    REQUIRE(budget->size() == 2);
    CHECK(wordsOf((*budget)[0]) == "the budget meeting moved to Friday");
    CHECK((*budget)[0].relevance > (*budget)[1].relevance);
    CHECK(index->matchText(buildTextQuery("catran"), 5, kBoth)->empty());
    CHECK(index->matchText(buildTextQuery("friday"), 5, {kA})->empty()); // not an allowed source
    CHECK(index->matchText(buildTextQuery("quotes \" inside"), 5, kBoth)); // never an FTS syntax error
}

TEST_CASE("The HNSW graph agrees with the exact scan, survives reopening and a damaged file", "[ai][search][index][hnsw]") {
    TempDir dir("hnsw");
    std::mt19937 rng(7);
    constexpr int kDim = 64;
    std::vector<IndexedKeyframe> a;
    std::vector<IndexedKeyframe> b;
    for (int i = 0; i < 600; ++i) {
        a.push_back(keyframe(i, i + 1, randomUnit(rng, kDim)));
        b.push_back(keyframe(i, i + 1, randomUnit(rng, kDim)));
    }
    const Vector query = randomUnit(rng, kDim);
    std::vector<VisualMatch> exact;
    {
        auto index = openIndex(dir.path, "random/1", kDim);
        REQUIRE(index->putKeyframes(kA, "A", a));
        REQUIRE(index->putKeyframes(kB, "B", b));
        auto scanned = index->nearest(query, 10, kBoth);
        REQUIRE(scanned);
        CHECK_FALSE(index->usedGraph());
        exact = *scanned;
    }
    const auto overlap = [&](const std::vector<VisualMatch>& found) {
        int same = 0;
        for (const VisualMatch& m : found) {
            same += std::ranges::any_of(exact, [&](const VisualMatch& e) { return e.fingerprint == m.fingerprint && e.range == m.range; }) ? 1 : 0;
        }
        return same;
    };
    SearchIndexOptions graph;
    graph.exactLimit = 0; // always the graph
    {
        auto index = openIndex(dir.path, "random/1", kDim, graph);
        auto found = index->nearest(query, 10, kBoth);
        REQUIRE(found);
        CHECK(index->usedGraph());
        CHECK(overlap(*found) >= 9); // approximate, but nearly exact at this size
        CHECK((*found)[0].fingerprint == exact[0].fingerprint);
        CHECK((*found)[0].range == exact[0].range);
        // Filtered to one source, the graph returns only it.
        auto onlyA = index->nearest(query, 10, {kA});
        REQUIRE(onlyA);
        CHECK(std::ranges::all_of(*onlyA, [](const VisualMatch& m) { return m.fingerprint == kA; }));
        // An update keeps the graph in step.
        REQUIRE(index->putKeyframes(kB, "B", std::vector<IndexedKeyframe>{keyframe(0, 1, query)}));
        auto updated = index->nearest(query, 1, kBoth);
        REQUIRE(updated);
        CHECK((*updated)[0].fingerprint == kB);
        CHECK((*updated)[0].similarity == Catch::Approx(1.0f));
        REQUIRE(index->flush());
    }
    CHECK(std::filesystem::exists(dir.path / "visual.hnsw"));
    {
        auto index = openIndex(dir.path, "random/1", kDim, graph); // the saved graph is loaded
        auto found = index->nearest(query, 1, kBoth);
        REQUIRE(found);
        CHECK(index->usedGraph());
        CHECK((*found)[0].fingerprint == kB);
        CHECK(std::filesystem::exists(dir.path / "visual.hnsw")); // it checked out (deleted elements and all)
    }
    std::ofstream(dir.path / "visual.hnsw", std::ios::binary | std::ios::trunc) << "garbage, not a graph";
    {
        auto index = openIndex(dir.path, "random/1", kDim, graph); // rebuilt from the database
        auto found = index->nearest(query, 1, kBoth);
        REQUIRE(found);
        CHECK((*found)[0].fingerprint == kB);
        CHECK(index->keyframeCount() == 601);
    }
}

TEST_CASE("A damaged index database is recreated (it is a cache)", "[ai][search][index]") {
    TempDir dir("damaged");
    {
        auto index = openIndex(dir.path);
        REQUIRE(index->putPhrases(kA, "A", "t", "en", std::vector<IndexedPhrase>{phrase({"hello", "there"}, 0)}));
    }
    std::filesystem::remove(dir.path / "index.sqlite-wal");
    std::filesystem::remove(dir.path / "index.sqlite-shm");
    std::ofstream(dir.path / "index.sqlite", std::ios::binary | std::ios::trunc) << std::string(4096, 'x');
    auto index = openIndex(dir.path);
    CHECK(index->sources().empty());
    REQUIRE(index->putPhrases(kA, "A", "t", "en", std::vector<IndexedPhrase>{phrase({"hello", "again"}, 0)}));
    CHECK(index->matchText(buildTextQuery("again"), 5, kBoth)->size() == 1);
}

TEST_CASE("Hybrid ranking fuses ranks and joins a phrase to the shot it falls in", "[ai][search][ranking]") {
    const std::vector<VisualMatch> visual{{kA, TimeRange::fromStartEnd(seconds(0), seconds(5)), seconds(2), 0.30f},
                                          {kB, TimeRange::fromStartEnd(seconds(10), seconds(15)), seconds(12), 0.25f}};
    TextMatch said;
    said.fingerprint = kB;
    said.range = TimeRange::fromStartEnd(seconds(13), seconds(17));
    said.words = {"a", "green", "boat"};
    said.wordTimes = {TimeRange::fromStartEnd(seconds(13), seconds(14)), TimeRange::fromStartEnd(seconds(14), seconds(15)),
                      TimeRange::fromStartEnd(seconds(15), seconds(17))};
    said.relevance = 2.0;
    TextMatch elsewhere = said;
    elsewhere.fingerprint = kA;
    elsewhere.range = TimeRange::fromStartEnd(seconds(30), seconds(33));
    elsewhere.wordTimes = {TimeRange::fromStartEnd(seconds(30), seconds(31)), TimeRange::fromStartEnd(seconds(31), seconds(32)),
                           TimeRange::fromStartEnd(seconds(32), seconds(33))};
    const std::vector<TextMatch> text{said, elsewhere};

    const auto hits = fuse(visual, text, buildTextQuery("green boat"));
    REQUIRE(hits.size() == 3);
    // B's shot was seen (rank 2) and heard (rank 1): first, spanning both.
    CHECK(hits[0].fingerprint == kB);
    CHECK(hits[0].score == Catch::Approx(1.0 / 62 + 1.0 / 61));
    CHECK(hits[0].range == TimeRange::fromStartEnd(seconds(10), seconds(17)));
    CHECK(hits[0].thumbnail == seconds(12)); // the matched frame
    REQUIRE(hits[0].similarity);
    REQUIRE(hits[0].relevance);
    CHECK(hits[0].matched == std::vector<std::size_t>{1, 2});
    // Then A's shot (visual rank 1), then the phrase that matched no shot (thumbnail at its first matched word).
    CHECK(hits[1].fingerprint == kA);
    CHECK(hits[1].score == Catch::Approx(1.0 / 61));
    CHECK_FALSE(hits[1].relevance);
    CHECK(hits[2].range.start == seconds(30));
    CHECK(hits[2].thumbnail == seconds(31));
    CHECK_FALSE(hits[2].similarity);

    // Weights tilt the fusion.
    FusionOptions wordsFirst;
    wordsFirst.transcriptWeight = 3.0;
    CHECK(fuse(visual, text, buildTextQuery("green boat"), wordsFirst)[1].fingerprint == kA);
    CHECK(fuse(visual, text, buildTextQuery("green boat"), wordsFirst)[1].relevance);
    CHECK(fuse({}, {}, buildTextQuery("x")).empty());
}

namespace {

/// The bytes of visual.hnsw, and a way to put others in its place WITH their SHA-256 in the database (as someone who
/// can write the cache could): only the index's own checks then stand between the file and hnswlib.
std::vector<std::uint8_t> graphBytes(const std::filesystem::path& dir) {
    std::ifstream in(dir / "visual.hnsw", std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), {});
}

void plantGraph(const std::filesystem::path& dir, const std::vector<std::uint8_t>& bytes) {
    std::ofstream(dir / "visual.hnsw", std::ios::binary | std::ios::trunc).write(reinterpret_cast<const char*>(bytes.data()),
                                                                                 static_cast<std::streamsize>(bytes.size()));
    ai::Sha256 h;
    h.update(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    const std::string hash = h.finishHex();
    sqlite3* db = nullptr;
    REQUIRE(sqlite3_open_v2((dir / "index.sqlite").string().c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK);
    const std::string sql = "UPDATE meta SET value = '" + hash + "' WHERE key = 'graph_sha256'";
    char* message = nullptr;
    const int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &message);
    INFO((message ? message : ""));
    sqlite3_free(message);
    sqlite3_close_v2(db);
    REQUIRE(rc == SQLITE_OK);
}

// hnswlib 0.9.0's saveIndex() layout: a 96-byte header, then each element's level-0 record (list, vector, label),
// then each element's upper-level lists behind a 32-bit byte count.
struct GraphLayout {
    std::size_t count = 0, perElement = 0, labelOffset = 0, dataOffset = 0, m = 0;
    std::vector<std::size_t> upperOffset; ///< where each element's upper lists start
    std::vector<int> levels;

    explicit GraphLayout(const std::vector<std::uint8_t>& b) {
        const auto sz = [&](std::size_t at) {
            std::size_t v = 0;
            std::memcpy(&v, b.data() + at, sizeof v);
            return v;
        };
        count = sz(16);
        perElement = sz(24);
        labelOffset = sz(32);
        dataOffset = sz(40);
        m = sz(72);
        std::size_t at = 96 + count * perElement;
        const std::size_t linksUpper = m * 4 + 4;
        for (std::size_t i = 0; i < count; ++i) {
            std::uint32_t size = 0;
            std::memcpy(&size, b.data() + at, 4);
            upperOffset.push_back(at + 4);
            levels.push_back(static_cast<int>(size / linksUpper));
            at += 4 + size;
        }
    }
    std::size_t element(std::size_t i) const { return 96 + i * perElement; }
};

template <class T>
void put(std::vector<std::uint8_t>& b, std::size_t at, T v) {
    std::memcpy(b.data() + at, &v, sizeof v);
}

} // namespace

TEST_CASE("A saved graph that does not check out never reaches hnswlib: refused, rebuilt, the answers right", "[ai][search][index][hnsw]") {
    TempDir dir("hnsw-hostile");
    std::mt19937 rng(11);
    constexpr int kDim = 32;
    std::vector<IndexedKeyframe> a;
    for (int i = 0; i < 800; ++i) {
        a.push_back(keyframe(i, i + 1, randomUnit(rng, kDim)));
    }
    const Vector query = randomUnit(rng, kDim);
    SearchIndexOptions graph;
    graph.exactLimit = 0;
    std::vector<VisualMatch> exact;
    {
        SearchIndexOptions scan;
        auto index = openIndex(dir.path, "random/1", kDim, scan);
        REQUIRE(index->putKeyframes(kA, "A", a));
        exact = *index->nearest(query, 5, {kA});
    }
    {
        auto index = openIndex(dir.path, "random/1", kDim, graph);
        REQUIRE(index->nearest(query, 5, {kA}));
        REQUIRE(index->flush());
    }
    const std::vector<std::uint8_t> good = graphBytes(dir.path);
    const GraphLayout layout(good);
    REQUIRE(layout.count == 800);
    REQUIRE(layout.m == 16);
    const auto upper = std::ranges::find_if(layout.levels, [](int l) { return l > 0; });
    const auto ground = std::ranges::find(layout.levels, 0);
    REQUIRE(upper != layout.levels.end());
    REQUIRE(ground != layout.levels.end());
    const std::size_t high = static_cast<std::size_t>(upper - layout.levels.begin());
    const std::size_t low = static_cast<std::size_t>(ground - layout.levels.begin());

    const std::vector<std::pair<const char*, std::function<void(std::vector<std::uint8_t>&)>>> hostile{
        {"an element count far beyond the file", [&](auto& b) { put<std::size_t>(b, 16, std::size_t{1} << 40); }},
        {"the label offset past the element", [&](auto& b) { put<std::size_t>(b, 32, layout.labelOffset + 4096); }},
        {"another M", [&](auto& b) { put<std::size_t>(b, 72, 8); }},
        {"the entry point out of range", [&](auto& b) { put<std::uint32_t>(b, 52, 5000); }},
        {"the entry point below the top level", [&](auto& b) { put<std::uint32_t>(b, 52, static_cast<std::uint32_t>(low)); }},
        {"a level-0 neighbour out of range", [&](auto& b) {
             put<std::uint16_t>(b, layout.element(3), 1);
             put<std::uint32_t>(b, layout.element(3) + 4, 900);
         }},
        {"a level-0 list longer than 2 M", [&](auto& b) { put<std::uint16_t>(b, layout.element(4), 33); }},
        {"a label that is no keyframe", [&](auto& b) { put<std::size_t>(b, layout.element(5) + layout.labelOffset, 987654321); }},
        {"two elements with one label", [&](auto& b) {
             std::size_t label = 0;
             std::memcpy(&label, b.data() + layout.element(6) + layout.labelOffset, sizeof label);
             put<std::size_t>(b, layout.element(7) + layout.labelOffset, label);
         }},
        {"a vector that is not the keyframe's", [&](auto& b) { put<float>(b, layout.element(8) + layout.dataOffset, 0.75f); }},
        {"a NaN in a vector", [&](auto& b) { put<float>(b, layout.element(9) + layout.dataOffset, std::numeric_limits<float>::quiet_NaN()); }},
        {"an upper-level neighbour without that level", [&](auto& b) {
             put<std::uint16_t>(b, layout.upperOffset[high], 1);
             put<std::uint32_t>(b, layout.upperOffset[high] + 4, static_cast<std::uint32_t>(low));
         }},
        {"upper lists of a size that is no whole number of lists", [&](auto& b) { put<std::uint32_t>(b, layout.upperOffset[high] - 4, 7); }},
        {"a truncated file", [&](auto& b) { b.resize(b.size() - 3); }},
        {"bytes after the last list", [&](auto& b) { b.push_back(0); }},
    };
    for (const auto& [what, damage] : hostile) {
        INFO(what);
        std::vector<std::uint8_t> bytes = good;
        damage(bytes);
        plantGraph(dir.path, bytes);
        auto index = openIndex(dir.path, "random/1", kDim, graph);
        auto found = index->nearest(query, 5, {kA});
        REQUIRE(found);
        CHECK(index->usedGraph());                                    // a graph rebuilt from the database
        CHECK_FALSE(std::filesystem::exists(dir.path / "visual.hnsw")); // the planted file is gone
        REQUIRE_FALSE(found->empty());
        CHECK((*found)[0].range == exact[0].range);
    }
    // The file as written checks out and is used.
    plantGraph(dir.path, good);
    auto index = openIndex(dir.path, "random/1", kDim, graph);
    auto found = index->nearest(query, 5, {kA});
    REQUIRE(found);
    CHECK(std::filesystem::exists(dir.path / "visual.hnsw"));
    CHECK((*found)[0].range == exact[0].range);
}

TEST_CASE("A graph whose upper-level lists fill up (the C.10 ASan finding: hnswlib prefetched past them)", "[ai][search][index][hnsw]") {
    // M = 4: upper-level lists hold four neighbours, so most of them are full; hnswlib's SSE prefetch read one past
    // the end of such a list while it built and searched (a heap-buffer-overflow read under AddressSanitizer). The
    // index builds hnswlib without those prefetches; under ASan this test is the reproducer.
    TempDir dir("hnsw-full");
    std::mt19937 rng(5);
    constexpr int kDim = 16;
    std::vector<IndexedKeyframe> a;
    for (int i = 0; i < 3000; ++i) {
        a.push_back(keyframe(i, i + 1, randomUnit(rng, kDim)));
    }
    SearchIndexOptions options;
    options.exactLimit = 0;
    options.hnswM = 4;
    options.hnswEfConstruction = 32;
    auto index = openIndex(dir.path, "random/1", kDim, options);
    REQUIRE(index->putKeyframes(kA, "A", a));
    for (int q = 0; q < 20; ++q) {
        const Vector query = randomUnit(rng, kDim);
        auto found = index->nearest(query, 8, {kA});
        REQUIRE(found);
        CHECK(found->size() == 8);
    }
    CHECK(index->usedGraph());
    REQUIRE(index->flush());
    auto reopened = openIndex(dir.path, "random/1", kDim, options);
    REQUIRE(reopened->nearest(randomUnit(rng, kDim), 8, {kA}));
    CHECK(std::filesystem::exists(dir.path / "visual.hnsw")); // the saved graph checked out
}

TEST_CASE("Search latency over 1,000 indexed shots", "[ai][search][benchmark][perf]") {
    // 50 files x 20 shots of SigLIP 2's 768 dimensions, and 2,000 transcript phrases; the query vector comes from a
    // fixture embedder (the model's own text encoding is measured by the real-model smoke test).
    TempDir dir("latency");
    constexpr int kDim = 768;
    std::mt19937 rng(42);
    auto index = std::shared_ptr<SearchIndex>(openIndex(dir.path, "bench/1", kDim).release());
    auto access = std::make_shared<FixtureAccess>();
    std::vector<MediaPtr> media;
    const std::vector<std::string> vocabulary{"budget", "meeting", "beach", "sunset", "interview", "drone", "city", "night",
                                              "coffee", "launch", "product", "team", "แมว", "ทะเล", "ตลาด", "กาแฟ"};
    for (std::uint64_t m = 1; m <= 50; ++m) {
        const std::uint64_t fingerprint = 0x1000 + m;
        media.push_back(mediaItem(m, "clip" + std::to_string(m) + ".mp4", 120));
        access->items[m].fingerprint = fingerprint;
        std::vector<IndexedKeyframe> shots;
        for (int s = 0; s < 20; ++s) {
            shots.push_back(keyframe(s * 6.0, s * 6.0 + 6.0, randomUnit(rng, kDim)));
        }
        REQUIRE(index->putKeyframes(fingerprint, "clip", shots));
        std::vector<IndexedPhrase> phrases;
        for (int p = 0; p < 40; ++p) {
            std::vector<std::string> words;
            for (int w = 0; w < 12; ++w) {
                words.push_back(vocabulary[rng() % vocabulary.size()]);
            }
            phrases.push_back(phrase(words, p * 3.0, 0.25));
        }
        REQUIRE(index->putPhrases(fingerprint, "clip", "t", m % 2 ? "en" : "th", phrases));
    }
    REQUIRE(index->keyframeCount() == 1000);
    REQUIRE(index->phraseCount() == 2000);

    struct FixedText : FixtureEmbedder {
        Vector query;
        std::expected<Vector, Error> embedText(std::string_view, std::stop_token) override { return query; }
        int dimension() const override { return kDim; }
    };
    auto embedder = std::make_shared<FixedText>();
    embedder->version = "bench/1";
    embedder->query = randomUnit(rng, kDim);
    FootageSearchOptions options;
    options.minSimilarity = -1.0f;
    FootageSearch search(options, index, access, embedder);
    const ProjectState state = projectWith(media);

    std::vector<double> times;
    for (int run = 0; run < 30; ++run) {
        const auto started = std::chrono::steady_clock::now();
        auto results = search.search(state, run % 2 ? "budget meeting" : "แมว ทะเล", SearchFilters{}, 20);
        times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
        REQUIRE(results);
        CHECK(results->hits.size() == 20);
        CHECK(results->visual);
        CHECK(results->mediaSearched == 50);
    }
    std::ranges::sort(times);
    const double median = times[times.size() / 2];
    const double p95 = times[times.size() * 95 / 100];
    WARN("smart search over 1,000 shots + 2,000 phrases: median " << median << " ms, p95 " << p95 << " ms (exact scan)");
    CHECK(median < 250.0); // generous: a debug build on a loaded CI machine; typically a few ms
}
