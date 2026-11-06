// AI video effects (0.7): the cache-backed source processor the evaluator asks (a hit gives the stored picture in place of
// the decoded one, a miss says so, Amount mixes), the cache identity of a media stream, and a whole run on a real file:
// decoder -> runner (fake backend) -> cache -> processor, frame by frame.

#include "VideoTestUtil.h"

#include <wabcut/ai/video/FrameConvert.h>
#include <wabcut/ai/video/MediaFrames.h>
#include <wabcut/ai/video/SourceProcessor.h>
#include <wabcut/media/VideoDecoder.h>

#include <catch2/catch_test_macros.hpp>

#include <fstream>

using namespace wabcut;
using namespace wabcut::ai::video;
using namespace wabcut::ai::video::test;

namespace {

MediaItem itemOf(const char* hash, std::int64_t size = 1000) {
    MediaItem item;
    item.id = MediaId{5};
    item.locator.path = "clip.mp4";
    item.locator.fingerprint.size = size;
    item.locator.fingerprint.sampleHash = hash;
    return item;
}

AiChain denoiseChain(int strength = 40, double amount = 1.0) {
    AiStage s;
    s.effect = std::string(kAiDenoiseEffect);
    s.denoise = strength;
    AiChain c;
    c.stages = {s};
    c.amount = amount;
    return c;
}

/// A decoded-looking 8-bit 4:2:0 frame of the given size, mid grey, starting at `pts`.
media::FrameHandle decoded(int w, int h, Ticks pts, Ticks duration) {
    auto data = std::make_shared<std::vector<std::vector<std::uint8_t>>>();
    data->push_back(std::vector<std::uint8_t>(static_cast<std::size_t>(w) * h, 100));
    data->push_back(std::vector<std::uint8_t>(static_cast<std::size_t>(w / 2) * (h / 2), 128));
    data->push_back(std::vector<std::uint8_t>(static_cast<std::size_t>(w / 2) * (h / 2), 128));
    media::FrameHandle f;
    f.pts = pts;
    f.duration = duration;
    f.width = w;
    f.height = h;
    f.format = media::PixelFormat::YUV420P;
    f.color.matrix = ColorMatrix::BT709;
    f.color.range = ColorRange::Limited;
    media::CpuPlanes cpu;
    cpu.planes[0] = media::Plane{(*data)[0].data(), w, w, h};
    cpu.planes[1] = media::Plane{(*data)[1].data(), w / 2, w / 2, h / 2};
    cpu.planes[2] = media::Plane{(*data)[2].data(), w / 2, w / 2, h / 2};
    cpu.owner = data;
    f.payload = std::move(cpu);
    return f;
}

/// Stores a flat 10-bit 4:4:4 picture of the given luma code value, 2x the decoded size, at `pts`.
void storeProcessed(ProcessedCache& cache, const CacheIdentity& id, const media::FrameHandle& like, int lumaCode, Ticks pts) {
    Picture grey;
    grey.width = like.width * 2;
    grey.height = like.height * 2;
    const std::uint16_t v = static_cast<std::uint16_t>(lumaCode * 65535 / 255);
    grey.rgb.assign(grey.pixels() * 3, v);
    auto stored = storedFrameOf(grey, like);
    REQUIRE(stored);
    stored->pts = pts.value;
    stored->duration = like.duration.value;
    REQUIRE(cache.put(id, *stored));
}

} // namespace

TEST_CASE("The cache identity follows the media's content, the stream and the chain", "[ai][video][processor]") {
    const MediaItem a = itemOf("00112233aabbccdd");
    const AiChain chain = denoiseChain();
    const CacheIdentity id = identityOf(a, 0, chain);
    CHECK(id.valid());
    CHECK(identityOf(a, 0, chain) == id);
    CHECK_FALSE(identityOf(a, 1, chain) == id);
    CHECK_FALSE(identityOf(itemOf("00112233aabbccde"), 0, chain) == id);
    CHECK_FALSE(identityOf(itemOf("00112233aabbccdd", 2000), 0, chain) == id);
    CHECK_FALSE(identityOf(a, 0, denoiseChain(70)) == id);
    CHECK(identityOf(a, 0, denoiseChain(40, 0.5)) == id); // Amount is not part of it
    CHECK_FALSE(identityOf(a, 0, AiChain{}).valid());
    // A copy of the file elsewhere (same fingerprint) shares the pictures; a path alone (no fingerprint) still identifies.
    MediaItem copy = a;
    copy.locator.path = "other/place/clip.mp4";
    CHECK(identityOf(copy, 0, chain) == id);
    MediaItem unprobed;
    unprobed.locator.path = "x.mp4";
    CHECK(identityOf(unprobed, 0, chain).valid());
    MediaItem generated = a;
    generated.generator = GeneratorSpec{};
    CHECK_FALSE(identityOf(generated, 0, chain).valid());
}

TEST_CASE("A stored picture replaces the decoded one; a missing one is reported", "[ai][video][processor]") {
    auto cache = std::make_shared<ProcessedCache>(freshDir("processor-basic"));
    CacheSourceProcessor processor(cache);
    const MediaItem item = itemOf("00112233aabbccdd");
    const AiChain chain = denoiseChain();
    const CacheIdentity id = identityOf(item, 0, chain);
    const Ticks duration = frameTicks(1);
    const media::FrameHandle d3 = decoded(32, 18, frameTicks(3), duration);
    storeProcessed(*cache, id, d3, 200, frameTicks(3));

    playback::SourceStageQuery q;
    q.item = &item;
    q.chain = &chain;
    q.decoded = &d3;
    q.sourceTime = frameTicks(3);
    auto hit = processor.process(q);
    CHECK_FALSE(hit.missing);
    REQUIRE(hit.frame.isValid());
    CHECK(hit.frame.width == 64);
    CHECK(hit.frame.height == 36);
    CHECK(hit.frame.format == media::PixelFormat::YUV444P10);
    CHECK(hit.frame.pts == frameTicks(3));
    CHECK(processor.hits() == 1);

    // The same query returns the very same planes: the compositor uploads them once.
    auto again = processor.process(q);
    REQUIRE(again.frame.isValid());
    CHECK(again.frame.cpu()->planes[0].data == hit.frame.cpu()->planes[0].data);

    // A neighbouring frame has no picture yet.
    const media::FrameHandle d4 = decoded(32, 18, frameTicks(4), duration);
    q.decoded = &d4;
    auto miss = processor.process(q);
    CHECK(miss.missing);
    CHECK_FALSE(miss.frame.isValid());
    CHECK(processor.misses() == 1);

    // A frame whose start differs by less than half a frame still finds it (variable frame rate timestamps).
    const media::FrameHandle near = decoded(32, 18, Ticks{frameTicks(3).value + duration.value / 3}, duration);
    q.decoded = &near;
    CHECK_FALSE(processor.process(q).missing);

    // Another chain has its own cache: nothing there.
    const AiChain other = denoiseChain(80);
    q.chain = &other;
    q.decoded = &d3;
    CHECK(processor.process(q).missing);

    // No chain, no item, no cache: nothing to say about a layer without AI effects.
    q.chain = nullptr;
    CHECK_FALSE(processor.process(q).missing);
    CacheSourceProcessor noCache(nullptr);
    q.chain = &chain;
    CHECK(noCache.process(q).missing);
}

TEST_CASE("Amount mixes the processed picture with the decoded one", "[ai][video][processor]") {
    auto cache = std::make_shared<ProcessedCache>(freshDir("processor-amount"));
    CacheSourceProcessor processor(cache);
    const MediaItem item = itemOf("00112233aabbccdd");
    const Ticks duration = frameTicks(1);
    const media::FrameHandle d0 = decoded(32, 18, frameTicks(0), duration); // luma code 100
    const AiChain full = denoiseChain(40, 1.0);
    storeProcessed(*cache, identityOf(item, 0, full), d0, 200, frameTicks(0)); // luma code 200

    const auto lumaOf = [&](double amount) {
        AiChain chain = denoiseChain(40, amount);
        playback::SourceStageQuery q;
        q.item = &item;
        q.chain = &chain;
        q.decoded = &d0;
        auto r = processor.process(q);
        REQUIRE(r.frame.isValid());
        REQUIRE_FALSE(r.missing);
        auto pic = pictureOf(r.frame);
        REQUIRE(pic);
        return pic->rgb[(5 * pic->width + 7) * 3 + 1];
    };
    const int atFull = lumaOf(1.0);
    const int atZero = lumaOf(0.05);
    const int atHalf = lumaOf(0.5);
    CHECK(atFull > atHalf);
    CHECK(atHalf > atZero);
    // Half way between the decoded grey (100) and the processed one (200).
    const int expectedHalf = (100 + 200) * 65535 / 255 / 2;
    CHECK(std::abs(atHalf - expectedHalf) < 700);
    // The mixed picture keeps the processed size; repeated asks reuse it.
    AiChain half = denoiseChain(40, 0.5);
    playback::SourceStageQuery q;
    q.item = &item;
    q.chain = &half;
    q.decoded = &d0;
    auto first = processor.process(q);
    auto second = processor.process(q);
    CHECK(first.frame.width == 64);
    CHECK(first.frame.cpu()->planes[0].data == second.frame.cpu()->planes[0].data);
}

TEST_CASE("installSourceProcessor makes the evaluator ask the cache", "[ai][video][processor]") {
    auto cache = std::make_shared<ProcessedCache>(freshDir("processor-install"));
    auto processor = installSourceProcessor(cache);
    CHECK(playback::sourceFrameProcessor() == processor);
    CHECK(processor->cache() == cache);
    playback::setSourceFrameProcessor(nullptr);
    CHECK(playback::sourceFrameProcessor() == nullptr);
}

TEST_CASE("A run on a real file: decode, process with the fake backend, serve the pictures to playback", "[ai][video][processor][media]") {
    const std::filesystem::path file = std::filesystem::path(WABCUT_TEST_MEDIA_DIR) / "h264_360p120.mp4";
    REQUIRE(std::filesystem::exists(file));
    auto probe = wabcut::media::VideoDecoder::open(file.string());
    REQUIRE(probe);
    const VideoStreamInfo& stream = (*probe)->streamInfo();

    MediaItem item = itemOf("feedfacecafef00d");
    item.locator.path = file.string();
    item.info.video.push_back(stream);

    AiStage restore;
    restore.effect = std::string(kAiRestoreEffect);
    restore.scale = 2;
    restore.quality = 1;
    AiChain chain;
    chain.stages = {restore};

    auto cache = std::make_shared<ProcessedCache>(freshDir("processor-real"));
    DecoderFrameSource source(file.string(), stream.index);
    FakeBackend backend;
    Runner runner(source, backend, *cache);
    Job job;
    job.identity = identityOf(item, 0, chain);
    job.chain = chain;
    job.range = TimeRange{Ticks{}, Ticks{Ticks::fromSeconds(1).value / 4}}; // a quarter of a second
    job.expectedFrames = expectedFrames(stream, job.range);
    REQUIRE(job.expectedFrames > 20);
    auto result = runner.run(job, {}, {});
    REQUIRE(result);
    CHECK(result->processed >= job.expectedFrames - 2);
    CHECK(result->processed <= job.expectedFrames + 2);
    CHECK(backend.calls.front().width == stream.width);

    // Playback decodes the same file through its own decoder and finds the pictures.
    CacheSourceProcessor processor(cache);
    auto playbackDecoder = wabcut::media::VideoDecoder::open(file.string(), stream.index);
    REQUIRE(playbackDecoder);
    int served = 0;
    int checked = 0;
    for (int n = 0; n < 20; ++n) {
        auto frame = (*playbackDecoder)->frameAt(Ticks{n * Ticks::fromSeconds(1).value / 120});
        REQUIRE(frame);
        playback::SourceStageQuery q;
        q.item = &item;
        q.chain = &chain;
        q.decoded = &*frame;
        q.sourceTime = frame->pts;
        const auto r = processor.process(q);
        ++checked;
        if (!r.missing) {
            ++served;
            CHECK(r.frame.width == stream.width * 2);
            CHECK(r.frame.height == stream.height * 2);
            CHECK(r.frame.pts == frame->pts);
        }
    }
    CHECK(served == checked); // every frame of the first part of the clip has its picture

    // A second run finds everything already there.
    auto again = runner.run(job, {}, {});
    REQUIRE(again);
    CHECK(again->processed == 0);
    CHECK(again->skipped == result->processed);
}

namespace {

/// Models "installed" the cheap way: files of the manifest's size (sparse) with the store's verified marker.
void installFakeModels(const std::filesystem::path& root, const AiChain& chain) {
    const ai::ModelStore store(root);
    for (const ai::ModelInfo* info : modelFilesOf(chain)) {
        REQUIRE(info != nullptr);
        const std::filesystem::path file = store.pathFor(*info);
        std::filesystem::create_directories(file.parent_path());
        {
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out << 'x';
        }
        std::filesystem::resize_file(file, info->size);
        std::filesystem::path marker = file;
        marker += ".verified";
        std::ofstream(marker, std::ios::trunc) << info->sha256;
    }
}

} // namespace

TEST_CASE("An export makes a missing picture on the spot; playback never does", "[ai][video][processor][export]") {
    auto cache = std::make_shared<ProcessedCache>(freshDir("processor-export"));
    CacheSourceProcessor processor(cache);
    const auto models = freshDir("processor-export-models");
    processor.setModelRoot(models);
    auto backend = std::make_shared<FakeBackend>();
    processor.setBackend(backend);
    const MediaItem item = itemOf("00112233aabbccdd");
    const AiChain chain = denoiseChain();
    installFakeModels(models, chain);
    const media::FrameHandle d0 = decoded(32, 18, frameTicks(0), frameTicks(1));
    playback::SourceStageQuery q;
    q.item = &item;
    q.chain = &chain;
    q.decoded = &d0;

    // Playback (the default mode): nothing is made, the picture is reported missing.
    CHECK(processor.process(q).missing);
    CHECK(backend->calls.empty());

    {
        playback::ScopedSourceStageMode scope(playback::SourceStageMode::ProcessMissing);
        auto made = processor.process(q);
        CHECK_FALSE(made.missing);
        REQUIRE(made.frame.isValid());
        CHECK(made.frame.width == 32);
        CHECK(made.frame.format == media::PixelFormat::YUV444P10);
        CHECK(backend->calls.size() == 1);
        CHECK_FALSE(scope.failure());
        // A second ask for the same frame is the cache's.
        auto again = processor.process(q);
        CHECK_FALSE(again.missing);
        CHECK(backend->calls.size() == 1);
    }
    // The picture stays: playback finds it afterwards.
    CHECK_FALSE(processor.process(q).missing);
    CHECK(cache->frames(identityOf(item, 0, chain)).size() == 1);
    // And the mode is gone with its scope.
    CHECK(playback::currentSourceStageMode() == playback::SourceStageMode::Show);
}

TEST_CASE("An export that cannot make a picture says why", "[ai][video][processor][export]") {
    const MediaItem item = itemOf("00112233aabbccdd");
    const AiChain chain = denoiseChain();
    const media::FrameHandle d0 = decoded(32, 18, frameTicks(0), frameTicks(1));
    playback::SourceStageQuery q;
    q.item = &item;
    q.chain = &chain;
    q.decoded = &d0;

    SECTION("the models are not downloaded") {
        auto cache = std::make_shared<ProcessedCache>(freshDir("processor-nomodel"));
        CacheSourceProcessor processor(cache);
        processor.setModelRoot(freshDir("processor-nomodel-store")); // empty
        auto backend = std::make_shared<FakeBackend>();
        processor.setBackend(backend);
        playback::ScopedSourceStageMode scope(playback::SourceStageMode::ProcessMissing);
        auto r = processor.process(q);
        CHECK(r.missing);
        REQUIRE(scope.failure());
        CHECK(scope.failure()->find("DRUNet") != std::string::npos);
        CHECK(scope.failure()->find("not downloaded") != std::string::npos);
        CHECK(scope.failure()->find("131 MB") != std::string::npos);
        CHECK(backend->calls.empty());
    }
    SECTION("there is no helper in this build") {
        auto cache = std::make_shared<ProcessedCache>(freshDir("processor-nohelper"));
        CacheSourceProcessor processor(cache);
        playback::ScopedSourceStageMode scope(playback::SourceStageMode::ProcessMissing);
        CHECK(processor.process(q).missing);
        REQUIRE(scope.failure());
        CHECK(scope.failure()->find("no AI helper") != std::string::npos);
    }
    SECTION("the model fails") {
        auto cache = std::make_shared<ProcessedCache>(freshDir("processor-fails"));
        CacheSourceProcessor processor(cache);
        const auto models = freshDir("processor-fails-models");
        processor.setModelRoot(models);
        installFakeModels(models, chain);
        auto backend = std::make_shared<FakeBackend>();
        backend->failAtCall = 0;
        processor.setBackend(backend);
        playback::ScopedSourceStageMode scope(playback::SourceStageMode::ProcessMissing);
        CHECK(processor.process(q).missing);
        REQUIRE(scope.failure());
        CHECK(scope.failure()->find("failed") != std::string::npos);
        CHECK(scope.failure()->find("helper crashed") != std::string::npos);
        CHECK(cache->frames(identityOf(item, 0, chain)).empty());
    }
    SECTION("a cancelled export is not a failure") {
        auto cache = std::make_shared<ProcessedCache>(freshDir("processor-cancelled"));
        CacheSourceProcessor processor(cache);
        const auto models = freshDir("processor-cancelled-models");
        processor.setModelRoot(models);
        installFakeModels(models, chain);
        processor.setBackend(std::make_shared<FakeBackend>());
        std::stop_source stop;
        stop.request_stop();
        playback::ScopedSourceStageMode scope(playback::SourceStageMode::ProcessMissing, stop.get_token());
        CHECK(processor.process(q).missing);
        CHECK_FALSE(scope.failure());
    }
    SECTION("scopes nest and restore the mode") {
        playback::ScopedSourceStageMode outer(playback::SourceStageMode::ProcessMissing);
        {
            playback::ScopedSourceStageMode inner(playback::SourceStageMode::Show);
            CHECK(playback::currentSourceStageMode() == playback::SourceStageMode::Show);
            playback::reportSourceStageFailure("inner");
            CHECK(inner.failure() == std::optional<std::string>("inner"));
        }
        CHECK(playback::currentSourceStageMode() == playback::SourceStageMode::ProcessMissing);
        CHECK_FALSE(outer.failure());
        playback::reportSourceStageFailure("first");
        playback::reportSourceStageFailure("second");
        CHECK(outer.failure() == std::optional<std::string>("first"));
    }
}
