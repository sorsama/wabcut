// P.1 analysis throughput (docs/performance.md): three pipelines, each as the app runs it.
//
//   * Shots: shot detection of perf_h264_1080p30.mp4 exactly as `wabcut-cli analyze <media> --shots --no-cache`
//     runs it (MediaAnalyzer, the default combined detector, no analysis cache): frames analysed per second and the
//     realtime factor of the analysis loop, the better of two runs. The decoder takes a hardware backend when there is
//     one (cuda / d3d11va; details.shots.decoder says which), hence [gpu].
//   * Search index: FootageSearch indexing 1,000 shots (50 files x 20 shots of 4 s, one keyframe each, plus 1,000
//     transcript phrases) through HelperEmbedder and the scripted wabcut_fake_embed helper at SigLIP 2's 768
//     dimensions, then 200 searches: shots indexed per second, and the search latency p50 / p95 (the query's text
//     vector from the helper, the exact scan, FTS5, the fusion). No model runs: the helper's vectors are mean colours,
//     so this is the pipeline's own cost (keyframe planning, resizing, the helper protocol, SQLite).
//   * Transcription: TranscriptionService on the clip's sound with the scripted wabcut_fake_infer helper: PCM
//     extraction (media::AudioDecoder to 16 kHz mono), the helper protocol, VAD chunking and the source-time mapping;
//     no model runs. With WABCUT_REAL_INFER (local-AI builds) and the tiny whisper model already in the model store
//     ($WABCUT_PERF_MODEL_DIR, else the user's; tests never download it), the real model transcribes the clip too.
//
// The helpers' paths come from WABCUT_FAKE_EMBED / WABCUT_FAKE_INFER; a build without one skips that part
// (details.skipped).

#include "PerfHarness.h"

#include <wabcut/ai/ModelStore.h>
#include <wabcut/ai/Sha256.h>
#include <wabcut/ai/Transcript.h>
#include <wabcut/ai/TranscriptionService.h>
#include <wabcut/ai/analysis/MediaAnalysis.h>
#include <wabcut/ai/analysis/ShotDetection.h>
#include <wabcut/ai/search/Embedding.h>
#include <wabcut/ai/search/FootageSearch.h>
#include <wabcut/ai/search/HelperEmbedder.h>
#include <wabcut/ai/search/SearchIndex.h>
#include <wabcut/model/ProjectState.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <stop_token>
#include <string>
#include <system_error>
#include <vector>

using namespace wabcut;

namespace {

using Clock = std::chrono::steady_clock;

std::string utf8(const std::filesystem::path& p) {
    const std::u8string u = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

/// An empty `dir`.
void freshDir(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
}

void removeDir(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// The helpers, when the suite was given their paths (runtime values, so every function stays referenced).
std::filesystem::path fakeEmbedHelper() {
#ifdef WABCUT_FAKE_EMBED
    return std::filesystem::path(WABCUT_FAKE_EMBED);
#else
    return {};
#endif
}

std::filesystem::path fakeInferHelper() {
#ifdef WABCUT_FAKE_INFER
    return std::filesystem::path(WABCUT_FAKE_INFER);
#else
    return {};
#endif
}

std::filesystem::path realInferHelper() {
#ifdef WABCUT_REAL_INFER
    return std::filesystem::path(WABCUT_REAL_INFER);
#else
    return {};
#endif
}

// ---- Shots ---------------------------------------------------------------------------------------------------------

void measureShots(const std::filesystem::path& clip, perf::PerfResult& r) {
    ai::analysis::MediaAnalyzerOptions options; // cacheRoot empty: no analysis cache, as --no-cache
    ai::analysis::MediaAnalyzer analyzer(options);
    const ai::analysis::ShotOptions shotOptions{}; // the CLI's defaults (--detector combined)
    ai::analysis::MediaAnalyzer::Stats best;
    std::int64_t frames = 0;
    std::size_t cuts = 0;
    double bestWall = 0.0;
    for (int run = 0; run < 2; ++run) {
        ai::analysis::MediaAnalyzer::Stats stats;
        const Clock::time_point started = Clock::now();
        const auto shots = analyzer.shots(utf8(clip), -1, shotOptions, {}, &stats);
        const double wall = perf::msSince(started) / 1000.0;
        INFO("shots: " << (shots ? std::string("ok") : shots.error().message));
        REQUIRE(shots.has_value());
        REQUIRE_FALSE(stats.fromCache);
        if (run == 0 || stats.seconds < best.seconds) {
            best = stats;
            frames = shots->frames;
            cuts = shots->cuts.size();
            bestWall = wall;
        }
    }
    REQUIRE(best.seconds > 0.0);
    const double mediaSeconds = best.mediaDuration.toSeconds();
    r.add("shots_fps", "fps", static_cast<double>(frames) / best.seconds, perf::Better::Higher);
    r.add("shots_x_realtime", "x realtime", mediaSeconds / best.seconds, perf::Better::Higher);
    r.note("shots_wall_s", "s", bestWall);
    r.details["shots"] = perf::Json{
        {"media", utf8(clip)},
        {"frames", frames},
        {"cuts", cuts},
        {"mediaSeconds", mediaSeconds},
        {"analysisSeconds", best.seconds},
        {"wallSeconds", bestWall},
        {"decoder", best.backend},
        {"detector", std::string(ai::analysis::toString(shotOptions.detector))},
        {"frameWidth", options.frameWidth},
        {"note", "fps and x realtime time the analysis loop (decode, shrink, detect), as the CLI reports it; the wall "
                 "time adds the source fingerprint and opening the decoder"}};
}

// ---- Search index ---------------------------------------------------------------------------------------------------

constexpr int kSearchFiles = 50;
constexpr int kShotsPerFile = 20;
constexpr std::int64_t kShotSeconds = 4; // under the 5 s keyframe interval: one keyframe per shot
constexpr int kPhrasesPerFile = 20;
constexpr int kWordsPerPhrase = 10;
constexpr int kSearchDimension = 768; // SigLIP 2 base
constexpr int kQueries = 200;

Ticks msTicks(std::int64_t ms) {
    return Ticks{ms * (kTicksPerSecond / 1000)};
}

/// The search benchmark's footage, in memory: 50 files of 20 shots of 4 s, each shot a plain colour (as the fake
/// helper embeds), and a transcript of 20 phrases of 10 words 2 s apart per file. Read-only after construction, so
/// thread-safe.
class BenchFootage final : public ai::search::SearchMediaAccess {
public:
    explicit BenchFootage(int frameWidth) : m_frameWidth(frameWidth) {
        for (int i = 0; i < 64; ++i) {
            m_pictures.push_back(picture(frameWidth, i));
        }
        m_shots.start = Ticks{};
        m_shots.end = Ticks::fromSeconds(kShotsPerFile * kShotSeconds);
        m_shots.frames = kShotsPerFile * kShotSeconds * 30;
        for (int s = 1; s < kShotsPerFile; ++s) {
            m_shots.cuts.push_back(ai::analysis::ShotCut{Ticks::fromSeconds(s * kShotSeconds), s * kShotSeconds * 30,
                                                         ai::analysis::CutKind::Cut, 0.9f});
        }
        const std::array<const char*, 16> vocabulary{"budget", "meeting", "beach",   "sunset", "interview", "drone",
                                                     "city",   "night",   "coffee",  "launch", "product",   "team",
                                                     "market", "river",   "morning", "crowd"};
        std::mt19937 rng(42);
        for (int f = 0; f < kSearchFiles; ++f) {
            auto item = std::make_shared<MediaItem>();
            item->id = m_state.ids.allocate<MediaId>();
            item->name = "clip" + std::to_string(f + 1) + ".mp4";
            item->info.duration = m_shots.end;
            VideoStreamInfo video;
            video.index = 0;
            video.width = 1920;
            video.height = 1080;
            video.duration = m_shots.end;
            item->info.video.push_back(video);
            AudioStreamInfo audio;
            audio.index = 1;
            audio.channels = 2;
            audio.duration = m_shots.end;
            item->info.audio.push_back(audio);

            auto spoken = std::make_shared<ai::Transcript>();
            spoken->language = "en";
            for (int p = 0; p < kPhrasesPerFile; ++p) {
                for (int w = 0; w < kWordsPerPhrase; ++w) {
                    ai::TranscriptWord word;
                    word.text = vocabulary[rng() % vocabulary.size()];
                    word.t0 = msTicks(p * 4000 + w * 200);
                    word.t1 = msTicks(p * 4000 + w * 200 + 180);
                    word.confidence = 0.9f;
                    spoken->words.push_back(std::move(word));
                }
            }
            m_transcripts[item->id.value] = std::move(spoken);
            m_state.media.push_back(std::move(item));
        }
    }

    const ProjectState& state() const { return m_state; }

    std::optional<std::uint64_t> fingerprint(const MediaItem& item) override {
        return 0x5EA2C00000000000ULL + item.id.value;
    }
    std::string date(const MediaItem&) override { return "2026-10-01"; }
    std::expected<ai::analysis::ShotAnalysis, Error> shots(const MediaItem&,
                                                           const ai::analysis::AnalysisProgress& progress) override {
        if (progress && !progress(1.0)) {
            return std::unexpected(Error(ErrorCode::Cancelled, "cancelled"));
        }
        return m_shots;
    }
    std::expected<std::vector<ai::search::Image>, Error> frames(const MediaItem& item, std::span<const Ticks> times,
                                                                int width, std::stop_token) override {
        std::vector<ai::search::Image> out;
        out.reserve(times.size());
        for (const Ticks t : times) {
            const auto shot = static_cast<std::uint64_t>(t.value / Ticks::fromSeconds(kShotSeconds).value);
            const auto colour = static_cast<std::size_t>((item.id.value * 31 + shot) % m_pictures.size());
            out.push_back(width == m_frameWidth ? m_pictures[colour] : picture(width, static_cast<int>(colour)));
        }
        return out;
    }
    std::shared_ptr<const ai::Transcript> transcript(const MediaItem& item) override {
        const auto it = m_transcripts.find(item.id.value);
        return it == m_transcripts.end() ? nullptr : it->second;
    }

private:
    /// A plain 16:9 picture of colour `index` (never black: the fake helper's vector of black is zero).
    static ai::search::Image picture(int width, int index) {
        ai::search::Image image;
        image.width = width;
        image.height = std::max(1, width * 9 / 16);
        const auto r = static_cast<std::uint8_t>(40 + (index * 67) % 200);
        const auto g = static_cast<std::uint8_t>(40 + (index * 131) % 200);
        const auto b = static_cast<std::uint8_t>(40 + (index * 29) % 200);
        image.rgb.resize(static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 3);
        for (std::size_t i = 0; i < image.rgb.size(); i += 3) {
            image.rgb[i] = r;
            image.rgb[i + 1] = g;
            image.rgb[i + 2] = b;
        }
        return image;
    }

    int m_frameWidth = 0;
    std::vector<ai::search::Image> m_pictures;
    ai::analysis::ShotAnalysis m_shots;
    std::map<std::uint64_t, std::shared_ptr<const ai::Transcript>> m_transcripts;
    ProjectState m_state;
};

void measureSearch(const std::filesystem::path& helper, const std::filesystem::path& dir, perf::PerfResult& r) {
    ai::search::HelperEmbedderOptions embedOptions;
    embedOptions.executable = helper;
    embedOptions.arguments = {"--mode", "normal", "--dim", std::to_string(kSearchDimension)};
    embedOptions.visionModel = "vision.onnx"; // the fake helper loads nothing
    embedOptions.textModel = "text.onnx";
    embedOptions.tokenizer = "tokenizer.json";
    embedOptions.modelVersion = "fake-embed/1";
    embedOptions.dimension = kSearchDimension;
    embedOptions.workDir = dir / "embed-work";
    auto embedder = std::make_shared<ai::search::HelperEmbedder>(embedOptions);

    // The helper's start and model load are not indexing: the app keeps the helper running between jobs.
    const Clock::time_point startedHelper = Clock::now();
    const auto warm = embedder->embedText("warm up", {});
    const double helperStartMs = perf::msSince(startedHelper);
    INFO("fake embed helper " << utf8(helper) << ": " << (warm ? std::string("ok") : warm.error().message));
    REQUIRE(warm.has_value());

    auto opened = ai::search::SearchIndex::open(dir / "index", embedder->modelVersion(), kSearchDimension);
    INFO("search index: " << (opened ? std::string("ok") : opened.error().message));
    REQUIRE(opened.has_value());
    const std::shared_ptr<ai::search::SearchIndex> index = std::move(*opened);

    const ai::search::FootageSearchOptions options{}; // no AnalysisCache: every keyframe is embedded
    const auto footage = std::make_shared<BenchFootage>(options.frameWidth);
    ai::search::FootageSearch engine(options, index, footage, embedder);
    const ProjectState& state = footage->state();

    const Clock::time_point startedIndex = Clock::now();
    const auto report = engine.index(state, {});
    const double indexSeconds = perf::msSince(startedIndex) / 1000.0;
    INFO("indexing: " << (report ? std::string("ok") : report.error().message));
    REQUIRE(report.has_value());
    INFO("errors: " << perf::Json(report->errors).dump());
    CHECK(report->failed == 0);
    CHECK(report->visual);
    CHECK(report->keyframes == static_cast<std::size_t>(kSearchFiles * kShotsPerFile));
    REQUIRE(indexSeconds > 0.0);
    r.add("search_index_shots_per_s", "shots/s", static_cast<double>(report->keyframes) / indexSeconds,
          perf::Better::Higher);
    r.note("search_index_s", "s", indexSeconds);
    r.note("search_helper_start_ms", "ms", helperStartMs);

    const std::array<const char*, 8> queries{"red budget meeting", "green beach at sunset", "blue city at night",
                                             "interview with the team", "product launch", "coffee in the morning",
                                             "drone over the river", "a crowd at the market"};
    for (std::size_t q = 0; q < 10; ++q) { // not measured: SQLite's page cache, the helper's first text request
        REQUIRE(engine.search(state, queries[q % queries.size()], ai::search::SearchFilters{}, 20).has_value());
    }
    perf::Samples latency;
    std::size_t hits = 0;
    std::size_t searched = 0;
    for (std::size_t q = 0; q < static_cast<std::size_t>(kQueries); ++q) {
        const Clock::time_point started = Clock::now();
        const auto results = engine.search(state, queries[q % queries.size()], ai::search::SearchFilters{}, 20);
        latency.add(perf::msSince(started));
        REQUIRE(results.has_value());
        hits += results->hits.size();
        searched = results->mediaSearched;
    }
    r.addSamples("search_query_p50_ms", "ms", latency, "p50");
    r.addSamples("search_query_p95_ms", "ms", latency, "p95");
    r.details["search"] = perf::Json{
        {"files", kSearchFiles},
        {"shots", report->keyframes},
        {"phrases", report->phrases},
        {"dimension", kSearchDimension},
        {"keyframeWidth", options.frameWidth},
        {"embedderDevice", embedder->device()},
        {"vectorSearch", index->usedGraph() ? "hnsw" : "exact scan"},
        {"queries", kQueries},
        {"hitsPerQuery", static_cast<double>(hits) / kQueries},
        {"mediaSearched", searched},
        {"indexSeconds", indexSeconds},
        {"helperStartMs", helperStartMs},
        {"note", "wabcut_fake_embed: no model runs (vectors are mean colours); measures keyframe planning, resizing, "
                 "the helper protocol, SQLite and FTS5 writes, and searches of 20 hits"}};
    embedder->shutdown();
}

// ---- Transcription --------------------------------------------------------------------------------------------------

/// A model "installed" in the store under `root`: a small file plus its verification marker (the fake helper loads
/// nothing).
ai::ModelInfo fakeModel(const std::filesystem::path& root, const std::string& id, ai::ModelKind kind) {
    const std::string body = "fake weights for " + id;
    ai::ModelInfo m{id, kind, id + ".bin", "https://example.invalid/" + id, body.size(), ai::Sha256::hex(body), "MIT",
                    "test", ""};
    std::filesystem::create_directories(root);
    std::ofstream(root / m.fileName, std::ios::binary) << body;
    std::ofstream(root / (m.fileName + ".verified"), std::ios::binary) << m.sha256 << '\n';
    return m;
}

void addTranscriptionDetails(const std::string& key, const ai::TranscriptionStats& s, std::size_t words,
                             perf::PerfResult& r) {
    r.details[key] = perf::Json{
        {"device", s.device},
        {"mediaSeconds", s.mediaSeconds},
        {"speechSeconds", s.speechSeconds},
        {"extractMs", s.extractMs},
        {"loadMs", s.loadMs},
        {"vadMs", s.vadMs},
        {"recognizeMs", s.recognizeMs},
        {"totalMs", s.totalMs},
        {"words", words}};
}

void measureFakeTranscription(const std::filesystem::path& helper, const std::filesystem::path& clip,
                              const std::filesystem::path& dir, perf::PerfResult& r) {
    ai::TranscriptionServiceConfig config;
    config.inferExecutable = helper;
    config.modelRoot = dir / "models";
    config.workDir = dir / "work";
    config.models = {fakeModel(config.modelRoot, "fake-whisper", ai::ModelKind::Whisper),
                     fakeModel(config.modelRoot, "fake-vad", ai::ModelKind::Vad)};
    // cacheRoot stays empty (no analysis cache); identify / extract stay unset: the real media seams.
    ai::TranscriptionService service(config);
    ai::TranscriptionRequest request;
    request.mediaPath = utf8(clip);
    request.options.model = "fake-whisper";
    request.options.vadModel = "fake-vad";
    request.options.useCache = false;
    ai::TranscriptionStats stats;
    request.onStats = [&stats](const ai::TranscriptionStats& s) { stats = s; };
    ai::TranscriptionStats best;
    std::size_t words = 0;
    for (int run = 0; run < 3; ++run) {
        stats = {};
        const auto transcript = service.run(request);
        INFO("fake transcription: " << (transcript ? std::string("ok") : transcript.error().message));
        REQUIRE(transcript.has_value());
        REQUIRE(stats.totalMs > 0.0);
        if (run == 0 || stats.totalMs < best.totalMs) {
            best = stats;
            words = transcript->words.size();
        }
    }
    r.add("transcribe_fake_x_realtime", "x realtime", best.mediaSeconds / (best.totalMs / 1000.0),
          perf::Better::Higher);
    r.note("transcribe_fake_total_ms", "ms", best.totalMs);
    r.note("transcribe_fake_extract_ms", "ms", best.extractMs);
    r.note("transcribe_fake_load_ms", "ms", best.loadMs);
    r.note("transcribe_fake_vad_ms", "ms", best.vadMs);
    addTranscriptionDetails("transcribeFake", best, words, r);
    r.details["transcribeFake"]["note"] =
        "wabcut_fake_infer: no model runs (speech is |sample| > 0.01, one word per second); measures PCM extraction "
        "to 16 kHz mono, the helper protocol, VAD chunking and the source-time mapping; the better of three runs";
}

/// The model store the real-model run looks in: $WABCUT_PERF_MODEL_DIR, else the user's. (The app tests' shell
/// fixture, when it is linked in, points WABCUT_CACHE_DIR and with it the default store at a temporary directory.)
std::filesystem::path realModelRoot() {
    if (const char* dir = std::getenv("WABCUT_PERF_MODEL_DIR"); dir != nullptr && *dir != '\0') {
        return std::filesystem::path(reinterpret_cast<const char8_t*>(dir));
    }
    return ai::ModelStore::defaultRoot();
}

/// The tiny whisper model through the real wabcut-infer, when it is in the model store (never downloaded here).
void measureTinyTranscription(const std::filesystem::path& helper, const std::filesystem::path& clip,
                              const std::filesystem::path& dir, perf::PerfResult& r) {
    const ai::ModelInfo* tiny = ai::findModel("tiny");
    const ai::ModelInfo* silero = ai::findModel("silero");
    const ai::ModelStore store(realModelRoot());
    if (tiny == nullptr || !store.isInstalled(*tiny)) {
        r.details["skipped"]["transcribe_tiny"] = "the tiny whisper model is not in " + utf8(store.root()) +
                                                  " (run `wabcut-cli transcribe <media> --model tiny` once, or set "
                                                  "WABCUT_PERF_MODEL_DIR to a model store)";
        return;
    }
    ai::TranscriptionServiceConfig config;
    config.inferExecutable = helper;
    config.modelRoot = store.root();
    config.workDir = dir / "work-tiny";
    ai::TranscriptionService service(config);
    ai::TranscriptionRequest request;
    request.mediaPath = utf8(clip);
    request.options.model = "tiny";
    request.options.language = "en";
    request.options.vad = silero != nullptr && store.isInstalled(*silero);
    request.options.useCache = false;
    ai::TranscriptionStats stats;
    request.onStats = [&stats](const ai::TranscriptionStats& s) { stats = s; };
    const auto transcript = service.run(request);
    INFO("tiny transcription: " << (transcript ? std::string("ok") : transcript.error().message));
    REQUIRE(transcript.has_value());
    REQUIRE(stats.totalMs > 0.0);
    r.add("transcribe_tiny_x_realtime", "x realtime", stats.mediaSeconds / (stats.totalMs / 1000.0),
          perf::Better::Higher);
    r.note("transcribe_tiny_total_ms", "ms", stats.totalMs);
    r.note("transcribe_tiny_load_ms", "ms", stats.loadMs);
    r.note("transcribe_tiny_recognize_ms", "ms", stats.recognizeMs);
    addTranscriptionDetails("transcribeTiny", stats, transcript->words.size(), r);
    r.details["transcribeTiny"]["vad"] = request.options.vad;
    r.details["transcribeTiny"]["note"] = "the real whisper.cpp tiny model, GPU allowed (the app's default), one run";
}

} // namespace

TEST_CASE("Benchmark analysis throughput (transcribe/shots/search index)", "[perf][gpu][analysis]") {
    const std::filesystem::path clip = std::filesystem::path(WABCUT_PERF_MEDIA_DIR) / "perf_h264_1080p30.mp4";
    if (!std::filesystem::is_regular_file(clip)) {
        SKIP("the benchmark media were not generated (tests/perf/gen_perf_bench_media.py --out " WABCUT_PERF_MEDIA_DIR
             ")");
    }
    const std::filesystem::path scratch = std::filesystem::path(perf::outputDir()) / "analysis-bench";
    const perf::BenchmarkOutcome outcome = perf::runBenchmark("analysis", [&] {
        perf::PerfResult r;
        r.details["skipped"] = perf::Json::object();
        measureShots(clip, r);

        if (const std::filesystem::path helper = fakeEmbedHelper(); !helper.empty()) {
            freshDir(scratch / "search");
            measureSearch(helper, scratch / "search", r);
            removeDir(scratch / "search");
        } else {
            r.details["skipped"]["search"] = "built without WABCUT_FAKE_EMBED (the path of wabcut_fake_embed)";
        }

        if (const std::filesystem::path helper = fakeInferHelper(); !helper.empty()) {
            freshDir(scratch / "transcribe");
            measureFakeTranscription(helper, clip, scratch / "transcribe", r);
            removeDir(scratch / "transcribe");
        } else {
            r.details["skipped"]["transcribe_fake"] = "built without WABCUT_FAKE_INFER (the path of wabcut_fake_infer)";
        }

        if (const std::filesystem::path helper = realInferHelper(); !helper.empty()) {
            freshDir(scratch / "transcribe-tiny");
            measureTinyTranscription(helper, clip, scratch / "transcribe-tiny", r);
            removeDir(scratch / "transcribe-tiny");
        } else {
            r.details["skipped"]["transcribe_tiny"] = "built without WABCUT_WITH_LOCAL_AI (no wabcut-infer)";
        }
        return r;
    });
    INFO(outcome.describe());
    CHECK(outcome.ok());
}
