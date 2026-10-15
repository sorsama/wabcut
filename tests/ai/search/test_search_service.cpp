// Smart search (AI-8) end to end on fixtures: FootageSearch indexing and searching (hybrid ranking, filters, re-indexing
// only what changed, embeddings reused from the AnalysisCache, transcripts alone without a model), the search.* commands
// through the engine, HelperEmbedder against a scripted helper (crash, hang, garbage, restart, cancel), and an optional
// smoke test of the real SigLIP 2 model in wabcut-embed (WABCUT_TEST_SIGLIP_DIR; the model is never downloaded here).

#include "CommandHarness.h"
#include "SearchTestUtil.h"

#include <wabcut/ai/AnalysisCache.h>
#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/ai/search/HelperEmbedder.h>
#include <wabcut/ai/search/SearchCommands.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>
#include <thread>

using namespace wabcut;
using namespace wabcut::ai::search;
using namespace wabcut::ai::search::test;
using command::Json;

namespace {

/// Three files: a beach clip (red, green, blue shots; "we walked along the beach" in the green one), an interview
/// (one grey 30 s take; the budget), a Thai voice memo (audio only).
struct World {
    explicit World(const std::string& name) : dir(name) {
        media = {mediaItem(1, "Beach.mp4", 20), mediaItem(2, "Interview.mov", 30), mediaItem(3, "Memo.wav", 12, false, true)};
        access = std::make_shared<FixtureAccess>();
        access->items[1] = FixtureAccess::Item{0x11, "2026-07-01", shotsAt(20, {5, 10}), {kRed, kGreen, kBlue},
                                               spoken("en", {{"we", 6, 6.3}, {"walked", 6.3, 6.8}, {"along", 6.8, 7.1},
                                                             {"the", 7.1, 7.2}, {"beach", 7.2, 7.8}}),
                                               false};
        access->items[2] = FixtureAccess::Item{0x22, "2026-09-15", shotsAt(30, {}), {kGrey},
                                               spoken("en", {{"the", 1, 1.2}, {"budget", 1.2, 1.8}, {"meeting", 1.8, 2.4},
                                                             {"is", 2.4, 2.5}, {"tomorrow", 2.5, 3.2}}),
                                               false};
        access->items[3] = FixtureAccess::Item{0x33, "2026-09-20", {}, {}, spoken("th", {{"แมว", 1, 1.5}, {"วิ่ง", 1.5, 2},
                                                                                           {"บน", 2, 2.2}, {"ชายหาด", 2.2, 3}}),
                                               false};
        embedder = std::make_shared<FixtureEmbedder>();
        options.cacheRoot = dir.path / "analysis";
        reopen();
    }
    void reopen(std::shared_ptr<Embedder> withEmbedder = nullptr, std::string indexName = "index") {
        auto opened = SearchIndex::open(dir.path / indexName, embedder->modelVersion(), kFixtureDim);
        REQUIRE(opened);
        index = std::shared_ptr<SearchIndex>(opened->release());
        search = std::make_shared<FootageSearch>(options, index, access, withEmbedder ? withEmbedder : embedder);
    }
    ProjectState state() const { return projectWith(media); }
    IndexReport indexAll() {
        auto report = search->index(state(), {});
        REQUIRE(report);
        return *report;
    }
    SearchResults find(std::string_view query, SearchFilters filters = {}, std::size_t limit = 10) {
        auto results = search->search(state(), query, filters, limit);
        REQUIRE(results);
        return *results;
    }

    TempDir dir;
    std::vector<MediaPtr> media;
    std::shared_ptr<FixtureAccess> access;
    std::shared_ptr<FixtureEmbedder> embedder;
    FootageSearchOptions options;
    std::shared_ptr<SearchIndex> index;
    std::shared_ptr<FootageSearch> search;
};

} // namespace

TEST_CASE("FootageSearch indexes shots and transcripts and finds what is shown and said", "[ai][search][service]") {
    World w("service");
    const IndexReport report = w.indexAll();
    CHECK(report.indexed == 3);
    CHECK(report.failed == 0);
    CHECK(report.visual);
    CHECK(report.keyframes == 4 + 6); // beach: 5 s, 5 s and a 10 s shot in two; interview: 30 s in six
    CHECK(report.phrases == 3);

    // What the frames show: the red shot first.
    SearchResults red = w.find("red");
    REQUIRE_FALSE(red.hits.empty());
    CHECK(red.visual);
    CHECK(red.hits[0].media == MediaId{1});
    CHECK(red.hits[0].mediaName == "Beach.mp4");
    CHECK(red.hits[0].range == TimeRange::fromStartEnd(seconds(0), seconds(5)));
    CHECK(red.hits[0].thumbnail == seconds(2.5));
    REQUIRE(red.hits[0].similarity);
    CHECK(*red.hits[0].similarity > 0.9f);

    // What is said, with the matched words.
    SearchResults budget = w.find("budget");
    REQUIRE_FALSE(budget.hits.empty());
    CHECK(budget.hits[0].media == MediaId{2});
    CHECK(budget.hits[0].snippet == "the budget meeting is tomorrow");
    CHECK(budget.hits[0].matchedWords == std::vector<std::string>{"budget"});
    CHECK(budget.hits[0].range.start == seconds(1));
    CHECK(budget.hits[0].thumbnail == seconds(1.2));

    // Seen and heard together ranks first: the green shot where they talk about the beach.
    SearchResults both = w.find("green beach");
    REQUIRE_FALSE(both.hits.empty());
    CHECK(both.hits[0].media == MediaId{1});
    CHECK(both.hits[0].range == TimeRange::fromStartEnd(seconds(5), seconds(10)));
    CHECK(both.hits[0].similarity);
    CHECK(both.hits[0].relevance);
    CHECK(both.hits[0].matchedWords == std::vector<std::string>{"beach"});

    // Thai, without spaces between the words searched for.
    SearchResults thai = w.find("แมววิ่ง");
    REQUIRE_FALSE(thai.hits.empty());
    CHECK(thai.hits[0].media == MediaId{3});
    CHECK(thai.hits[0].snippet == "แมววิ่งบนชายหาด");
    CHECK(thai.hits[0].matchedWords == std::vector<std::string>{"แมว", "วิ่ง"});

    // A query nothing matches: no visual hit (below the similarity floor), no words.
    CHECK(w.find("bicycle").hits.empty());
    CHECK(w.search->search(w.state(), "   ", SearchFilters{}, 10).error().code == ErrorCode::InvalidArgument);

    const SearchStatus status = w.search->status(w.state());
    CHECK(status.media == 3);
    CHECK(status.indexed == 3);
    CHECK(status.visual == 2);
    CHECK(status.transcript == 3);
    CHECK(status.visualAvailable);
}

TEST_CASE("Search filters: name, date, duration, media, visual or words only", "[ai][search][service]") {
    World w("filters");
    w.indexAll();
    SearchFilters byName;
    byName.name = "INTER";
    CHECK(w.find("the", byName).hits.size() == 1);
    CHECK(w.find("the", byName).hits[0].media == MediaId{2});

    SearchFilters byDate;
    byDate.from = "2026-09-01";
    byDate.to = "2026-09-16";
    const auto dated = w.find("the", byDate);
    REQUIRE(dated.hits.size() == 1);
    CHECK(dated.hits[0].media == MediaId{2});

    SearchFilters longOnes;
    longOnes.minDuration = seconds(25); // the red shot's file is 20 s: only the interview's grey take is left
    const auto longHits = w.find("red", longOnes);
    CHECK_FALSE(longHits.hits.empty());
    CHECK(std::ranges::all_of(longHits.hits, [](const SearchHit& h) { return h.media == MediaId{2}; }));
    SearchFilters shortOnes;
    shortOnes.maxDuration = seconds(25);
    CHECK_FALSE(w.find("red", shortOnes).hits.empty());

    SearchFilters onlyMemo;
    onlyMemo.media = {MediaId{3}};
    CHECK(w.find("beach", onlyMemo).hits.empty()); // the beach clip is not in scope
    const auto memo = w.find("ชายหาด", onlyMemo);
    REQUIRE(memo.hits.size() == 1);
    CHECK(memo.hits[0].media == MediaId{3});
    SearchFilters wordsOnly;
    wordsOnly.visual = false;
    const auto noFrames = w.find("red beach", wordsOnly);
    CHECK_FALSE(noFrames.visual);
    CHECK(std::ranges::none_of(noFrames.hits, [](const SearchHit& h) { return h.similarity.has_value(); }));
    SearchFilters framesOnly;
    framesOnly.transcript = false;
    CHECK(std::ranges::none_of(w.find("green beach", framesOnly).hits, [](const SearchHit& h) { return h.relevance.has_value(); }));
}

TEST_CASE("Indexing again touches only what changed; embeddings come back from the AnalysisCache", "[ai][search][service]") {
    World w("reindex");
    w.indexAll();
    const int embedded = w.embedder->imagesEmbedded;
    const int decoded = w.access->frameCalls;

    // Nothing changed: nothing is decoded or embedded.
    IndexReport again = w.indexAll();
    CHECK(again.upToDate == 3);
    CHECK(again.indexed == 0);
    CHECK(w.embedder->imagesEmbedded == embedded);
    CHECK(w.access->frameCalls == decoded);

    // A new media item is reported until it is indexed.
    w.media.push_back(mediaItem(4, "New.mp4", 10));
    w.access->items[4] = FixtureAccess::Item{0x44, "2026-10-01", shotsAt(10, {}), {kBlue}, nullptr, false};
    SearchResults before = w.find("blue");
    CHECK(before.notIndexed == std::vector<MediaId>{MediaId{4}});
    CHECK_FALSE(before.warnings.empty());
    std::vector<MediaId> onlyNew{MediaId{4}};
    auto one = w.search->index(w.state(), onlyNew);
    REQUIRE(one);
    CHECK(one->indexed == 1);
    CHECK(w.find("blue").notIndexed.empty());

    // The transcript changed: its phrases are replaced.
    w.access->items[2].transcript = spoken("en", {{"the", 1, 1.2}, {"forecast", 1.2, 1.8}});
    IndexReport edited = w.indexAll();
    CHECK(edited.indexed == 1);
    CHECK(edited.phrases == 1);
    CHECK(w.find("budget").hits.empty());
    CHECK_FALSE(w.find("forecast").hits.empty());

    // The file changed (a new fingerprint): indexed anew; the old entry is never returned for this project.
    w.access->items[1].fingerprint = 0x111;
    w.access->items[1].shotColours = {kBlue, kBlue, kBlue};
    IndexReport changed = w.indexAll();
    CHECK(changed.indexed == 1);
    const auto red = w.find("red");
    CHECK(std::ranges::none_of(red.hits, [](const SearchHit& h) { return h.fingerprint == 0x11; }));

    // A fresh index (another machine's cache directory, a wiped index) restores the embeddings from the
    // AnalysisCache: no frame is decoded and no picture embedded again.
    const int embeddedBefore = w.embedder->imagesEmbedded;
    const int decodedBefore = w.access->frameCalls;
    w.reopen(nullptr, "index-fresh");
    IndexReport restored = w.indexAll();
    CHECK(restored.indexed == 4);
    CHECK(restored.keyframes > 0);
    CHECK(w.embedder->imagesEmbedded == embeddedBefore);
    CHECK(w.access->frameCalls == decodedBefore);
    CHECK_FALSE(w.find("blue").hits.empty());
    // The payload is in the cache under the file's fingerprint and the model version.
    const ai::AnalysisCache cache(w.options.cacheRoot);
    const auto exported = cache.exportSidecar(0x22);
    REQUIRE(exported);
    CHECK(exported->find(std::string(kKeyframeAnalyzer)) != std::string::npos);
    CHECK(exported->find("fixture-colours/1") != std::string::npos);
}

TEST_CASE("Without an embedding model, transcripts are still indexed and searched", "[ai][search][service]") {
    World w("nomodel");
    w.search = std::make_shared<FootageSearch>(w.options, w.index, w.access, nullptr);
    w.search->setVisualUnavailableReason("the SigLIP 2 model is not downloaded");
    const IndexReport report = w.indexAll();
    CHECK_FALSE(report.visual);
    CHECK(report.keyframes == 0);
    CHECK(report.phrases == 3);
    CHECK(w.access->frameCalls == 0);
    const auto results = w.find("beach");
    CHECK_FALSE(results.visual);
    REQUIRE_FALSE(results.hits.empty());
    CHECK(results.hits[0].media == MediaId{1});
    REQUIRE_FALSE(results.warnings.empty());
    CHECK(results.warnings.back() == "visual search is off: the SigLIP 2 model is not downloaded");
    CHECK_FALSE(w.search->status(w.state()).visualAvailable);
}

TEST_CASE("Indexing reports offline files, cancels on request and survives a failing decoder", "[ai][search][service]") {
    World w("failures");
    w.access->items[2].offline = true;
    IndexReport report = w.indexAll();
    CHECK(report.failed == 1);
    REQUIRE(report.errors.size() == 1);
    CHECK(report.errors[0] == "'Interview.mov': the file is offline");

    World c("cancel");
    int calls = 0;
    auto cancelled = c.search->index(c.state(), {}, [&](const IndexProgress& p) {
        CHECK(p.count == 3);
        CHECK(p.fraction >= 0.0);
        CHECK(p.fraction <= 1.0);
        return ++calls < 3;
    });
    REQUIRE_FALSE(cancelled);
    CHECK(cancelled.error().code == ErrorCode::Cancelled);
    std::stop_source stop;
    stop.request_stop();
    CHECK(c.search->index(c.state(), {}, {}, stop.get_token()).error().code == ErrorCode::Cancelled);

    struct Broken : FixtureAccess {
        std::expected<std::vector<Image>, Error> frames(const MediaItem&, std::span<const Ticks>, int, std::stop_token) override {
            return std::unexpected(Error(ErrorCode::DecoderError, "corrupt frame"));
        }
    };
    World b("broken");
    auto broken = std::make_shared<Broken>();
    broken->items = b.access->items;
    b.search = std::make_shared<FootageSearch>(b.options, b.index, broken, b.embedder);
    IndexReport partial = *b.search->index(b.state(), {});
    CHECK(partial.failed == 2); // both videos; the memo (no video) indexes
    CHECK_FALSE(b.find("แมว").hits.empty());
}

// --- Commands ------------------------------------------------------------------------------------------------------------

TEST_CASE("search.footage and search.indexMedia through the engine; a hit inserts its source range", "[ai][search][commands]") {
    TempDir dir("commands");
    auto access = std::make_shared<FixtureAccess>();
    auto embedder = std::make_shared<FixtureEmbedder>();
    auto index = SearchIndex::open(dir.path, embedder->modelVersion(), kFixtureDim);
    REQUIRE(index);
    auto search = std::make_shared<FootageSearch>(FootageSearchOptions{}, std::shared_ptr<SearchIndex>(index->release()), access, embedder);
    auto bundle = std::make_shared<command::ServiceBundle>(
        std::vector<std::shared_ptr<command::CommandServices>>{std::make_shared<SearchServices>(search)});
    wabcut::test::CommandHarness h(R"(
        V1 tgt |A10aaaaaaaaaa|
        A1 tgt |A10aaaaaaaaaa|
    )",
                                   command::EnginePolicy{command::CapabilitySet::all(), 1024, bundle}, ai::edit::editingCommands());
    const MediaId a = h.tl.media('A');
    // Media A: 4 s red then 4 s blue (the fixture's media letters are long; its shots end at 8 s).
    access->items[a.value] = FixtureAccess::Item{0xAA, "2026-10-01", shotsAt(8, {4}), {kRed, kBlue},
                                                 spoken("en", {{"look", 4.5, 5}, {"blue", 5, 5.5}, {"sky", 5.5, 6}}), false};

    const auto* footage = ai::edit::editingCommands().find("search.footage");
    REQUIRE(footage);
    CHECK(footage->capability == command::Capability::Read);
    CHECK(ai::edit::editingCommands().find("search.indexMedia")->capability == command::Capability::MediaAnalysis);

    const auto indexed = h.ok("search.indexMedia");
    CHECK(indexed.data["indexed"] == 1);
    CHECK(indexed.data["keyframes"] == 2);
    CHECK(indexed.data["visual"] == true);
    CHECK_FALSE(indexed.committed); // an analysis job, not an edit

    const auto found = h.ok("search.footage", Json{{"query", "blue sky"}, {"limit", 5}});
    INFO(found.data.dump());
    REQUIRE(found.data["hits"].size() >= 1);
    const Json hit = found.data["hits"][0];
    CHECK(hit["media"] == a.value);
    CHECK(hit["mediaName"] == "A");
    CHECK(hit["sourceIn"] == seconds(4).value);
    CHECK(hit["sourceOut"] == seconds(8).value);
    CHECK(hit["start"] == 4.0);
    CHECK(hit["transcript"] == true);
    CHECK(hit["matchedWords"] == Json::array({"blue", "sky"}));
    CHECK(hit.contains("similarity"));
    CHECK(found.data["visual"] == true);
    CHECK_FALSE(found.committed);

    // What an agent does next: insert the hit's range at the playhead.
    const auto inserted = h.ok("clip.insert", Json{{"media", hit["media"]}, {"at", wabcut::test::tc(0)}, {"sourceIn", hit["sourceIn"]},
                                                   {"sourceOut", hit["sourceOut"]}});
    CHECK(inserted.committed);

    CHECK(h.errorOf("search.footage", Json{{"query", "x"}, {"from", "yesterday"}}) == "InvalidParams");
    CHECK(h.errorOf("search.footage", Json::object()) == "InvalidParams"); // the query is required

    // A read-only client may search but not index; a host without the services can do neither.
    wabcut::test::CommandHarness readOnly("V1 tgt |A10aaaa|", command::EnginePolicy{command::CapabilitySet::of({command::Capability::Read}), 1024, bundle},
                                          ai::edit::editingCommands());
    CHECK(readOnly.errorOf("search.footage", Json{{"query", "blue"}}).empty());
    CHECK(readOnly.errorOf("search.indexMedia") == "PermissionDenied");
    wabcut::test::CommandHarness none("V1 tgt |A10aaaa|", command::EnginePolicy{}, ai::edit::editingCommands());
    CHECK(none.errorOf("search.footage", Json{{"query", "blue"}}) == "InvalidRequest");
}

// --- HelperEmbedder against the scripted helper ----------------------------------------------------------------------------

namespace {

HelperEmbedderOptions fakeHelper(const std::string& mode, std::vector<std::string> extra = {}) {
    HelperEmbedderOptions o;
    o.executable = WABCUT_FAKE_EMBED;
    o.arguments = {"--mode", mode, "--dim", std::to_string(kFixtureDim)};
    o.arguments.insert(o.arguments.end(), extra.begin(), extra.end());
    o.visionModel = "vision.onnx";
    o.textModel = "text.onnx";
    o.tokenizer = "tokenizer.json";
    o.modelVersion = "fake/1";
    o.dimension = kFixtureDim;
    o.inputSize = 32;
    o.batchSize = 2;
    o.startTimeout = std::chrono::seconds(10);
    o.loadTimeout = std::chrono::seconds(10);
    o.requestTimeout = std::chrono::seconds(10);
    return o;
}

std::vector<Image> colours() {
    return {solid(40, 30, 230, 10, 10), solid(64, 36, 10, 220, 10), solid(20, 20, 10, 10, 230), solid(32, 32, 230, 10, 10),
            solid(48, 27, 10, 10, 230)};
}

} // namespace

TEST_CASE("HelperEmbedder embeds through the helper in batches and keeps it running", "[ai][search][helper]") {
    TempDir dir("helper");
    HelperEmbedder embedder(fakeHelper("normal", {"--starts", (dir.path / "starts.txt").string()}));
    auto vectors = embedder.embedImages(colours(), {});
    REQUIRE(vectors);
    REQUIRE(vectors->size() == 5); // three requests of at most two pictures
    for (const Vector& v : *vectors) {
        CHECK(dot(v, v) == Catch::Approx(1.0f));
    }
    CHECK((*vectors)[0][0] > 0.9f); // red
    CHECK((*vectors)[1][1] > 0.9f); // green
    CHECK((*vectors)[4][2] > 0.9f); // blue
    auto blue = embedder.embedText("a blue sky", {});
    REQUIRE(blue);
    CHECK((*blue)[2] == Catch::Approx(1.0f));
    CHECK(embedder.device() == "FakeCPU");
    CHECK(embedder.starts() == 1);
    CHECK(embedder.embedImages({}, {})->empty());
    embedder.shutdown();
    REQUIRE(embedder.embedText("red", {}));
    CHECK(embedder.starts() == 2); // started again after shutdown()
}

TEST_CASE("HelperEmbedder turns helper crashes, hangs and protocol breaches into errors and recovers", "[ai][search][helper]") {
    TempDir dir("helper-faults");
    {
        HelperEmbedder crash(fakeHelper("crash-on-embed"));
        auto failed = crash.embedText("red", {});
        REQUIRE_FALSE(failed);
        CHECK(failed.error().code == ErrorCode::DecoderError);
        CHECK(failed.error().message.find("exit code 57") != std::string::npos);
    }
    {
        // Crashes once, then works: the next call starts a new helper.
        HelperEmbedder once(fakeHelper("crash-once", {"--marker", (dir.path / "crashed").string()}));
        CHECK_FALSE(once.embedImages(colours(), {}));
        auto retried = once.embedImages(colours(), {});
        REQUIRE(retried);
        CHECK(retried->size() == 5);
        CHECK(once.starts() == 2);
    }
    {
        auto options = fakeHelper("hang-on-embed");
        options.requestTimeout = std::chrono::milliseconds(800);
        HelperEmbedder hang(options);
        const auto started = std::chrono::steady_clock::now();
        auto failed = hang.embedText("red", {});
        REQUIRE_FALSE(failed);
        CHECK(failed.error().message.find("did not answer within") != std::string::npos);
        CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(8));
    }
    {
        HelperEmbedder hang(fakeHelper("hang-on-embed"));
        std::stop_source stop;
        std::thread canceller([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            stop.request_stop();
        });
        const auto started = std::chrono::steady_clock::now();
        auto cancelled = hang.embedImages(colours(), stop.get_token());
        canceller.join();
        REQUIRE_FALSE(cancelled);
        CHECK(cancelled.error().code == ErrorCode::Cancelled);
        CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(5));
    }
    for (const auto& [mode, expected] : std::vector<std::pair<std::string, std::string>>{
             {"garbage", "invalid message"},
             {"wrong-dim", "wrong dimension"},
             {"wrong-count", "vectors for"},
             {"wrong-protocol", "protocol 99"},
             {"fail-load", "no such model file"},
             {"crash-at-start", "start"}}) {
        INFO(mode);
        HelperEmbedder broken(fakeHelper(mode));
        auto failed = broken.embedImages(colours(), {});
        REQUIRE_FALSE(failed);
        CHECK(failed.error().message.find(expected) != std::string::npos);
    }
    auto missing = fakeHelper("normal");
    missing.executable = dir.path / "no-such-helper.exe";
    HelperEmbedder none(missing);
    CHECK(none.embedText("red", {}).error().code == ErrorCode::NotFound);
}

// --- The real model (optional) ---------------------------------------------------------------------------------------------

TEST_CASE("SigLIP 2 in wabcut-embed tells red, green and blue apart (WABCUT_TEST_SIGLIP_DIR)", "[ai][search][model]") {
#ifndef WABCUT_EMBED_HELPER
    SKIP("built without WABCUT_WITH_ONNX: no wabcut-embed");
#else
    const char* dir = std::getenv("WABCUT_TEST_SIGLIP_DIR");
    if (dir == nullptr || *dir == '\0') {
        SKIP("set WABCUT_TEST_SIGLIP_DIR to the folder with SigLIP 2's vision_model_quantized.onnx, "
             "text_model_quantized.onnx and tokenizer.json");
    }
    HelperEmbedderOptions options;
    options.executable = WABCUT_EMBED_HELPER;
    options.visionModel = std::filesystem::path(dir) / "vision_model_quantized.onnx";
    options.textModel = std::filesystem::path(dir) / "text_model_quantized.onnx";
    options.tokenizer = std::filesystem::path(dir) / "tokenizer.json";
    options.modelVersion = siglipModelVersion();
    if (const char* device = std::getenv("WABCUT_TEST_EMBED_DEVICE")) {
        options.device = device;
    }
    HelperEmbedder embedder(options);
    const std::vector<Image> pictures{solid(384, 216, 220, 30, 30), solid(384, 216, 30, 190, 40), solid(384, 216, 30, 40, 220)};
    auto started = std::chrono::steady_clock::now();
    auto images = embedder.embedImages(pictures, {});
    REQUIRE(images);
    const double imageMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    std::vector<Vector> texts;
    double textMs = 0;
    for (const char* text : {"a red picture", "a green picture", "a blue picture"}) {
        started = std::chrono::steady_clock::now();
        auto v = embedder.embedText(text, {});
        textMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        REQUIRE(v);
        texts.push_back(*v);
    }
    for (std::size_t i = 0; i < 3; ++i) {
        INFO("picture " << i);
        std::size_t best = 0;
        for (std::size_t t = 1; t < 3; ++t) {
            if (dot((*images)[i], texts[t]) > dot((*images)[i], texts[best])) {
                best = t;
            }
        }
        CHECK(best == i);
    }
    // Indexing throughput: 32 keyframes (two batches of 16), the model already loaded.
    std::vector<Image> batch;
    for (int i = 0; i < 32; ++i) {
        batch.push_back(solid(384, 216, static_cast<std::uint8_t>(i * 7), static_cast<std::uint8_t>(255 - i * 5), 90));
    }
    started = std::chrono::steady_clock::now();
    REQUIRE(embedder.embedImages(batch, {}));
    const double perPicture = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count() / 32.0;
    WARN("SigLIP 2 on " << embedder.device() << ": first 3 pictures (with load) " << imageMs << " ms, one text " << textMs
                        << " ms, " << perPicture << " ms per keyframe in batches of 16; red/red " << dot((*images)[0], texts[0])
                        << ", red/blue " << dot((*images)[0], texts[2]));
#endif
}

// Review wave 4, P2-9: a redaction hides part of a picture everywhere, smart search included. Keyframes inside a
// redacted range are never decoded, embedded, cached or indexed; ones indexed before the redaction are not returned,
// and the next indexing replaces them and drops the whole-picture embeddings from the AnalysisCache.
TEST_CASE("Smart search leaves redacted ranges out of the visual index and the cache", "[ai][search][service]") {
    World w("redaction");
    w.indexAll();
    REQUIRE_FALSE(w.find("red").hits.empty()); // the beach clip's red shot, 0 .. 5 s
    ProjectState redacted = w.state();
    Redaction r;
    r.id = RedactionId{900};
    r.media = MediaId{1};
    r.source = TimeRange::fromStartEnd(seconds(0), seconds(5));
    redacted.redactions.push_back(r);
    const auto visualRed = [](const SearchResults& results) {
        return std::ranges::any_of(results.hits, [](const SearchHit& h) { return h.media == MediaId{1} && h.similarity && h.range.start < seconds(5); });
    };
    // Before indexing again: the stale keyframe is not a hit.
    auto stale = w.search->search(redacted, "red", SearchFilters{}, 10);
    REQUIRE(stale);
    CHECK_FALSE(visualRed(*stale));
    // Indexing again: only the beach file, without its red shot.
    const int embedded = w.embedder->imagesEmbedded;
    auto report = w.search->index(redacted, {});
    REQUIRE(report);
    CHECK(report->indexed == 1);
    CHECK(w.embedder->imagesEmbedded == embedded + 3); // green, and the blue shot's two stretches
    auto again = w.search->search(redacted, "red", SearchFilters{}, 10);
    REQUIRE(again);
    CHECK_FALSE(visualRed(*again));
    CHECK_FALSE(w.search->search(redacted, "green", SearchFilters{}, 10)->hits.empty()); // the rest is still found
    // Up to date for the redacted project; the whole-picture embeddings are gone from the cache.
    auto unchanged = w.search->index(redacted, {});
    REQUIRE(unchanged);
    CHECK(unchanged->upToDate == 3);
    const ai::AnalysisCache cache(w.options.cacheRoot);
    const auto exported = cache.exportSidecar(0x11);
    REQUIRE(exported);
    const auto json = nlohmann::json::parse(*exported, nullptr, false);
    std::size_t keyframeEntries = 0;
    for (const auto& entry : json.value("entries", nlohmann::json::array())) {
        keyframeEntries += entry.value("key", nlohmann::json::object()).value("analyzer", std::string()) == kKeyframeAnalyzer ? 1 : 0;
    }
    CHECK(keyframeEntries == 1); // only the redacted variant remains
}
