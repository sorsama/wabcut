// AI video effects (0.7): the runner with a fake backend and a fake frame source - no model, no media. A clip's frames go
// through a chain into the cache with progress; frames the cache has are skipped without being converted; a cancel in
// the middle keeps what was made and a second run continues; a chain's steps apply in order with their strength, grain,
// resize and alpha; a missing model or a failing helper ends the run with its message and leaves the cache consistent.

#include "VideoTestUtil.h"

#include <wabcut/ai/video/Runner.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <stop_token>

using namespace wabcut;
using namespace wabcut::ai::video;
using namespace wabcut::ai::video::test;

namespace {

AiStage restoreStage(int scale, int quality, int deblur, int denoise, int grain = 0) {
    AiStage s;
    s.effect = std::string(kAiRestoreEffect);
    s.scale = scale;
    s.quality = quality;
    s.deblur = deblur;
    s.denoise = denoise;
    s.grain = grain;
    return s;
}

AiStage denoiseStage(int strength, int grain = 0) {
    AiStage s;
    s.effect = std::string(kAiDenoiseEffect);
    s.denoise = strength;
    s.grain = grain;
    return s;
}

Job jobFor(const AiChain& chain, int frames, int width = 16, int height = 12) {
    Job job;
    job.identity = identity(aiChainHash(chain));
    job.chain = chain;
    job.range = TimeRange{Ticks{}, frameTicks(frames)};
    job.expectedFrames = frames;
    job.sourceWidth = width;
    job.sourceHeight = height;
    return job;
}

AiChain chainOf(std::initializer_list<AiStage> stages) {
    AiChain c;
    c.stages = stages;
    return c;
}

} // namespace

TEST_CASE("A run processes every frame of the range into the cache and reports progress", "[ai][video][runner]") {
    ProcessedCache cache(freshDir("runner-basic"));
    FakeSource source(10, 16, 12);
    FakeBackend backend;
    Runner runner(source, backend, cache);
    const AiChain chain = chainOf({denoiseStage(40)});
    const Job job = jobFor(chain, 10);

    std::vector<Progress> seen;
    auto result = runner.run(job, [&](const Progress& p) { seen.push_back(p); }, {});
    REQUIRE(result);
    CHECK(result->processed == 10);
    CHECK(result->skipped == 0);
    CHECK_FALSE(result->cancelled);
    CHECK(result->device == "FakeGPU");
    CHECK(cache.frames(job.identity).size() == 10);
    CHECK(backend.calls.size() == 10);
    REQUIRE(seen.size() >= 11);
    CHECK(seen.front().processed == 0);
    CHECK(seen.back().processed == 10);
    CHECK(seen.back().total == 10);
    CHECK(seen.back().secondsLeft == 0.0);
    CHECK(seen.back().device == "FakeGPU");
    for (std::size_t i = 1; i < seen.size(); ++i) {
        CHECK(seen[i].processed >= seen[i - 1].processed); // never goes back
    }
    // The denoise mark is in the stored picture, and the frame's timing was set by the runner.
    auto frame = cache.get(job.identity, frameTicks(3), Ticks{1});
    REQUIRE(frame);
    CHECK(frame->pts == frameTicks(3).value);
    CHECK(frame->duration == frameTicks(1).value);
    CHECK(frame->width == 16);
    const Picture original = pattern(16, 12, 3);
    CHECK(frame->planes[0].data[0] == static_cast<std::uint8_t>(std::min(original.rgb[0] + 1000, 65535) >> 8));
}

TEST_CASE("A second run skips what the cache has without converting those frames", "[ai][video][runner]") {
    ProcessedCache cache(freshDir("runner-skip"));
    FakeSource source(8, 16, 12);
    FakeBackend backend;
    Runner runner(source, backend, cache);
    const Job job = jobFor(chainOf({denoiseStage(40)}), 8);
    REQUIRE(runner.run(job, {}, {}));
    CHECK(source.pictureCalls == 8);

    auto again = runner.run(job, {}, {});
    REQUIRE(again);
    CHECK(again->processed == 0);
    CHECK(again->skipped == 8);
    CHECK(source.pictureCalls == 8); // not converted again
    CHECK(backend.calls.size() == 8);

    // A longer range processes only the new frames; another chain starts from nothing.
    FakeSource longer(12, 16, 12);
    Runner runner2(longer, backend, cache);
    auto more = runner2.run(jobFor(chainOf({denoiseStage(40)}), 12), {}, {});
    REQUIRE(more);
    CHECK(more->processed == 4);
    CHECK(more->skipped == 8);
    auto other = runner2.run(jobFor(chainOf({denoiseStage(70)}), 12), {}, {});
    REQUIRE(other);
    CHECK(other->processed == 12);
}

TEST_CASE("Cancel in the middle keeps what was made and the next run continues", "[ai][video][runner]") {
    ProcessedCache cache(freshDir("runner-cancel"));
    FakeSource source(10, 16, 12);
    FakeBackend backend;
    Runner runner(source, backend, cache);
    const Job job = jobFor(chainOf({denoiseStage(40)}), 10);

    std::stop_source stop;
    auto first = runner.run(job, [&](const Progress& p) { if (p.processed == 3) stop.request_stop(); }, stop.get_token());
    REQUIRE(first);
    CHECK(first->cancelled);
    CHECK(first->processed == 3);
    CHECK(cache.frames(job.identity).size() == 3);

    auto second = runner.run(job, {}, {});
    REQUIRE(second);
    CHECK_FALSE(second->cancelled);
    CHECK(second->skipped == 3);
    CHECK(second->processed == 7);
    CHECK(cache.frames(job.identity).size() == 10);

    // A cancel before the first frame does nothing at all.
    ProcessedCache fresh(freshDir("runner-cancel-early"));
    Runner idle(source, backend, fresh);
    std::stop_source now;
    now.request_stop();
    auto none = idle.run(job, {}, now.get_token());
    REQUIRE(none);
    CHECK(none->cancelled);
    CHECK(none->processed == 0);
    CHECK(fresh.frames(job.identity).empty());
}

TEST_CASE("A cancel that reaches the backend between two steps ends the run without a half picture", "[ai][video][runner]") {
    ProcessedCache cache(freshDir("runner-cancel-backend"));
    FakeSource source(6, 16, 12);
    FakeBackend backend;
    std::stop_source stop;
    // Two steps per frame (denoise, deblur): the cancel arrives after the denoise of the second frame, the deblur is refused.
    backend.onCall = [&](int index) { if (index == 2) stop.request_stop(); };
    Runner runner(source, backend, cache);
    const Job job = jobFor(chainOf({restoreStage(1, 0, 100, 100)}), 6);
    auto result = runner.run(job, {}, stop.get_token());
    REQUIRE(result);
    CHECK(result->cancelled);
    CHECK(cache.frames(job.identity).size() == 1); // the first frame only: the second never reached the cache
    CHECK(result->processed == 1);
}

TEST_CASE("The steps of a stage apply in order with their strength, grain and resize", "[ai][video][runner]") {
    ProcessedCache cache(freshDir("runner-steps"));
    FakeSource source(1, 8, 6);
    FakeBackend backend;
    Runner runner(source, backend, cache);
    const Picture in = pattern(8, 6, 0);

    SECTION("denoise, deblur, then the 2x upscaler") {
        auto out = runner.process(in, chainOf({restoreStage(2, 1, 100, 100)}), {});
        REQUIRE(out);
        REQUIRE(backend.calls.size() == 3);
        CHECK(backend.calls[0].model == Model::Denoise);
        CHECK(backend.calls[1].model == Model::Deblur);
        CHECK(backend.calls[2].model == Model::Upscale2x);
        CHECK(backend.calls[2].width == 8); // each step sees the previous step's picture
        CHECK(out->width == 16);
        CHECK(out->height == 12);
        const int expected = std::min(in.rgb[0] + 1000 + 2000, 65535);
        CHECK(std::abs(static_cast<int>(out->rgb[0]) - expected) <= 1);
    }
    SECTION("deblur at half strength mixes half the change in") {
        auto out = runner.process(in, chainOf({restoreStage(1, 0, 50, 0)}), {});
        REQUIRE(out);
        CHECK(std::abs(static_cast<int>(out->rgb[0]) - (in.rgb[0] + 1000)) <= 1); // +2000 at half strength
    }
    SECTION("grain puts part of the removed noise back") {
        auto none = runner.process(in, chainOf({denoiseStage(100, 0)}), {});
        auto half = runner.process(in, chainOf({denoiseStage(100, 50)}), {});
        auto all = runner.process(in, chainOf({denoiseStage(100, 100)}), {});
        REQUIRE(none);
        REQUIRE(half);
        REQUIRE(all);
        CHECK(std::abs(static_cast<int>(none->rgb[0]) - (in.rgb[0] + 1000)) <= 1);
        CHECK(std::abs(static_cast<int>(half->rgb[0]) - (in.rgb[0] + 500)) <= 1);
        CHECK(std::abs(static_cast<int>(all->rgb[0]) - in.rgb[0]) <= 1); // all the noise back: the input
    }
    SECTION("the fast 2x runs the 4x model and halves the result") {
        auto out = runner.process(in, chainOf({restoreStage(2, 0, 0, 0)}), {});
        REQUIRE(out);
        REQUIRE(backend.calls.size() == 1);
        CHECK(backend.calls[0].model == Model::UpscaleFast4x);
        CHECK(out->width == 16);
        CHECK(out->height == 12);
        CHECK(std::abs(static_cast<int>(out->rgb[0]) - in.rgb[0]) <= 1); // nearest 4x then a 2 x 2 mean of equal pixels
    }
    SECTION("the tile and the noise level reach the backend") {
        AiStage s = restoreStage(1, 0, 0, 100);
        s.tile = 128;
        auto out = runner.process(in, chainOf({s}), {});
        REQUIRE(out);
        REQUIRE(backend.calls.size() == 1);
        CHECK(backend.calls[0].tile == 128);
        CHECK(backend.calls[0].sigma == Catch::Approx(25.0 / 255.0));
    }
}

TEST_CASE("Alpha is resized to the result and the models never see it", "[ai][video][runner]") {
    ProcessedCache cache(freshDir("runner-alpha"));
    FakeSource source(1, 8, 6, true);
    FakeBackend backend;
    Runner runner(source, backend, cache);
    const Picture in = pattern(8, 6, 0, true);
    REQUIRE(in.hasAlpha());
    auto out = runner.process(in, chainOf({restoreStage(2, 1, 0, 0)}), {});
    REQUIRE(out);
    REQUIRE(out->hasAlpha());
    CHECK(out->alpha.size() == 16u * 12u);
    CHECK(out->valid());
    CHECK(out->alpha[0] == in.alpha[0]);

    // Same size: the alpha plane is the input's.
    FakeBackend plain;
    Runner runner2(source, plain, cache);
    auto same = runner2.process(in, chainOf({denoiseStage(40)}), {});
    REQUIRE(same);
    CHECK(same->alpha == in.alpha);
}

TEST_CASE("A missing model or a failing helper ends the run with its message and leaves the cache consistent", "[ai][video][runner]") {
    SECTION("the model is not downloaded") {
        ProcessedCache cache(freshDir("runner-missing"));
        FakeSource source(5, 16, 12);
        FakeBackend backend;
        backend.failAtCall = 0;
        backend.failure = ErrorCode::NotFound;
        Runner runner(source, backend, cache);
        const Job job = jobFor(chainOf({denoiseStage(40)}), 5);
        auto result = runner.run(job, {}, {});
        REQUIRE_FALSE(result);
        CHECK(result.error().code == ErrorCode::NotFound);
        CHECK(result.error().message.find("not downloaded") != std::string::npos);
        CHECK(cache.frames(job.identity).empty());
    }
    SECTION("the helper fails on the fourth frame") {
        ProcessedCache cache(freshDir("runner-crash"));
        FakeSource source(8, 16, 12);
        FakeBackend backend;
        backend.failAtCall = 3;
        Runner runner(source, backend, cache);
        const Job job = jobFor(chainOf({denoiseStage(40)}), 8);
        auto result = runner.run(job, {}, {});
        REQUIRE_FALSE(result);
        CHECK(cache.frames(job.identity).size() == 3); // what was made stays
        backend.failAtCall = -1;
        auto resumed = runner.run(job, {}, {});
        REQUIRE(resumed);
        CHECK(resumed->skipped == 3);
        CHECK(resumed->processed == 5);
    }
    SECTION("the backend answers with a picture of the wrong size") {
        ProcessedCache cache(freshDir("runner-wrongsize"));
        FakeSource source(2, 16, 12);
        FakeBackend backend;
        backend.wrongSizeAtCall = 0;
        Runner runner(source, backend, cache);
        const Job job = jobFor(chainOf({restoreStage(2, 1, 0, 0)}), 2);
        auto result = runner.run(job, {}, {});
        REQUIRE_FALSE(result);
        CHECK(result.error().code == ErrorCode::DecoderError);
        CHECK(cache.frames(job.identity).empty());
    }
    SECTION("the decoder fails") {
        ProcessedCache cache(freshDir("runner-decode"));
        FakeSource source(6, 16, 12);
        source.failFrame = 2;
        FakeBackend backend;
        Runner runner(source, backend, cache);
        const Job job = jobFor(chainOf({denoiseStage(40)}), 6);
        auto result = runner.run(job, {}, {});
        REQUIRE_FALSE(result);
        CHECK(result.error().message.find("scripted decode failure") != std::string::npos);
        CHECK(cache.frames(job.identity).size() == 2);
    }
}

TEST_CASE("A run refuses an empty job and stops where the media ends", "[ai][video][runner]") {
    ProcessedCache cache(freshDir("runner-edges"));
    FakeSource source(4, 16, 12);
    FakeBackend backend;
    Runner runner(source, backend, cache);
    Job job = jobFor(chainOf({denoiseStage(40)}), 4);
    Job empty = job;
    empty.chain = AiChain{};
    CHECK_FALSE(runner.run(empty, {}, {}));
    Job noRange = job;
    noRange.range = TimeRange{};
    CHECK_FALSE(runner.run(noRange, {}, {}));
    Job noIdentity = job;
    noIdentity.identity = CacheIdentity{};
    CHECK_FALSE(runner.run(noIdentity, {}, {}));
    // A range longer than the media: the media's frames only.
    job.range = TimeRange{Ticks{}, frameTicks(100)};
    auto result = runner.run(job, {}, {});
    REQUIRE(result);
    CHECK(result->processed == 4);
    // The provenance is written when the run has one.
    Job withProvenance = jobFor(chainOf({denoiseStage(55)}), 2);
    withProvenance.provenance.chain = "Denoise";
    withProvenance.provenance.created = "2026-11-05T00:00:00Z";
    REQUIRE(runner.run(withProvenance, {}, {}));
    auto provenance = cache.provenance(withProvenance.identity);
    REQUIRE(provenance);
    CHECK(provenance->chain == "Denoise");
}

TEST_CASE("The runner records the speed of each model and the estimate uses it", "[ai][video][runner]") {
    ProcessedCache cache(freshDir("runner-speed"));
    FakeSource source(2, 100, 100);
    FakeBackend backend;
    Runner runner(source, backend, cache);
    const AiChain chain = chainOf({denoiseStage(40)});
    const double guess = estimateSeconds(chain, 1920, 1080, 100, &cache, true);
    CHECK(guess > 0.0);
    REQUIRE(runner.run(jobFor(chain, 2, 100, 100), {}, {}));
    // The fake reports 10 ms for a 0.01 MP picture: 1000 ms per megapixel.
    CHECK(cache.speed("drunet-color", "FakeGPU") == Catch::Approx(1000.0));
    // The estimate looks the speed up by the device the models run on ("DirectML"), so a measured FakeGPU changes nothing.
    CHECK(estimateSeconds(chain, 1920, 1080, 100, &cache, true) == Catch::Approx(guess));
    cache.recordSpeed("drunet-color", "DirectML", 500.0);
    const double measured = estimateSeconds(chain, 1920, 1080, 100, &cache, true);
    CHECK(measured == Catch::Approx(500.0 * 1920.0 * 1080.0 / 1e6 * 100.0 / 1000.0));
    CHECK(estimateSeconds(AiChain{}, 1920, 1080, 100, &cache, true) == 0.0);
    CHECK(estimateSeconds(chain, 1920, 1080, 0, &cache, true) == 0.0);

    CHECK(durationText(12.0) == "under a minute");
    CHECK(durationText(250.0) == "about 4 min");
    CHECK(durationText(4800.0) == "about 1 h 20 min");
    CHECK(durationText(7200.0) == "about 2 h");
    CHECK(durationText(-1.0).empty());
}
