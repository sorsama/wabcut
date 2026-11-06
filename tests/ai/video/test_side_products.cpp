// AI Frame Interpolation (card 133) and AI Refocus (card 181) on the AI video runner: the two models are pinned downloads with
// the licences the library admits; a depth map is normalised, inverted to a distance and stored as one 8-bit plane at the
// model's working size; an interpolated picture is keyed by its exact source time in 1/64 of a frame and made from the two
// frames around it (a time on a frame stores a stub); the runner skips what the cache has, resumes and reports failures; a
// clip's targets list one key per output frame. Fake backend and frame source: no model, no media, no helper.

#include "VideoTestUtil.h"

#include <wabcut/ai/ModelStore.h>
#include <wabcut/ai/video/ModelKeys.h>
#include <wabcut/ai/video/Picture.h>
#include <wabcut/ai/video/Runner.h>
#include <wabcut/ai/video/Targets.h>
#include <wabcut/ai/video/VideoModels.h>
#include <wabcut/model/Clip.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/model/Sequence.h>
#include <wabcut/model/TimeInterpolation.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <stop_token>
#include <utility>

using namespace wabcut;
using namespace wabcut::ai;
using namespace wabcut::ai::video;
using namespace wabcut::ai::video::test;
using Catch::Approx;

namespace {

Job depthJob(int frames) {
    Job job;
    job.kind = JobKind::Depth;
    job.chain = aiDepthChain();
    job.identity = identity(aiChainHash(job.chain));
    job.range = TimeRange{Ticks{}, frameTicks(frames)};
    job.expectedFrames = frames;
    return job;
}

Job interpolationJob(std::vector<Ticks> times) {
    Job job;
    job.kind = JobKind::Interpolation;
    job.chain = aiInterpolationChain();
    job.identity = identity(aiChainHash(job.chain));
    job.times = std::move(times);
    job.framePeriod = frameTicks(1);
    job.expectedFrames = static_cast<int>(job.times.size());
    return job;
}

Ticks frameAndFraction(std::int64_t frame, int quarters) { return frameTicks(frame) + Ticks{frameTicks(1).value / 4 * quarters}; }

std::shared_ptr<MediaItem> mediaOf(ProjectState& s, int w, int h) {
    auto item = std::make_shared<MediaItem>();
    item->id = s.ids.allocate<MediaId>();
    item->locator.path = "clip.mp4";
    item->locator.fingerprint.size = 4000;
    item->locator.fingerprint.sampleHash = "00112233aabbccdd";
    VideoStreamInfo v;
    v.index = 0;
    v.width = w;
    v.height = h;
    v.frameRate = Rate{25, 1};
    v.duration = Ticks::fromSeconds(10);
    item->info.video.push_back(v);
    item->info.duration = v.duration;
    return item;
}

Sequence sequenceOf(ProjectState& s, std::vector<Clip> clips) {
    Sequence seq;
    seq.id = s.ids.allocate<SequenceId>();
    seq.frameRate = Rate{25, 1};
    seq.width = 1920;
    seq.height = 1080;
    Track t;
    t.id = s.ids.allocate<TrackId>();
    t.clips = ClipList(std::move(clips));
    seq.videoTracks.push_back(std::make_shared<const Track>(std::move(t)));
    return seq;
}

} // namespace

TEST_CASE("The frame interpolation and depth models are verified, pinned downloads (cards 133 and 181)", "[ai][video][side]") {
    for (const char* id : {"video-interpolate-rife", "video-depth-anything-v2-small"}) {
        INFO(id);
        const ModelInfo* info = findModel(id);
        REQUIRE(info != nullptr);
        CHECK(info->kind == ModelKind::Video);
        CHECK(isAllowedModelLicense(info->license));
        CHECK(info->sha256.size() == 64);
        CHECK(info->size > 1'000'000);
        CHECK(info->licenseCard.size() > 80);
        CHECK(info->fileName.starts_with("ai-video/"));
        const std::size_t resolve = info->url.find("/resolve/");
        REQUIRE(resolve != std::string::npos);
        const std::string revision = info->url.substr(resolve + 9, 40); // pinned to a commit, never "main"
        CHECK(revision.find_first_not_of("0123456789abcdef") == std::string::npos);
        CHECK(revision.size() == 40);
    }
    CHECK(findModel("video-interpolate-rife")->license == "MIT");
    CHECK(findModel("video-depth-anything-v2-small")->license == "Apache-2.0"); // the Base/Large models are CC-BY-NC: not used
    CHECK(describe(Model::Interpolate).key == kKeyInterpolate);
    CHECK(describe(Model::Interpolate).multiple == 64);
    CHECK(describe(Model::Interpolate).scale == 1);
    CHECK(describe(Model::Depth).key == kKeyDepth);
    CHECK(describe(Model::Depth).multiple == 14);
    CHECK(describe(Model::Depth).scale == 1);
}

TEST_CASE("The depth and interpolation chains name their model, their cache and the download", "[ai][video][side]") {
    const AiChain depth = aiDepthChain();
    const AiChain interpolation = aiInterpolationChain();
    REQUIRE(stepsOf(depth).size() == 1);
    CHECK(stepsOf(depth).front().model == Model::Depth);
    REQUIRE(stepsOf(interpolation).size() == 1);
    CHECK(stepsOf(interpolation).front().model == Model::Interpolate);
    CHECK(aiChainHash(depth) != 0);
    CHECK(aiChainHash(interpolation) != 0);
    CHECK(aiChainHash(depth) != aiChainHash(interpolation)); // their pictures never share a folder
    CHECK(aiChainLabel(depth) == "Depth maps");
    CHECK(aiChainLabel(interpolation) == "Frame interpolation");
    const std::string version = modelVersion(depth);
    CHECK(version.starts_with("depth-anything-v2-small-afb6a5c2"));
    CHECK(modelVersion(interpolation).starts_with("rife-v425-lite-702802d0"));
    const ModelStore empty(freshDir("side-no-models"));
    CHECK(missingBytes(depth, empty) == 99060839);
    CHECK(missingBytes(interpolation, empty) == 22677513);
    CHECK_FALSE(modelsInstalled(depth, empty));
    CHECK(downloadCard(depth, empty).find("Apache") != std::string::npos);
    CHECK(downloadCard(interpolation, empty).find("MIT") != std::string::npos);
    // The depth stage is a side product: it never replaces the decoded picture, and is never a no-op.
    CHECK(depth.stages.front().isSideProduct());
    CHECK_FALSE(depth.stages.front().isNoop());
    CHECK(depth.outputScale() == 1);
}

TEST_CASE("A depth model's answer is normalised by percentiles and stored as a distance", "[ai][video][side]") {
    // 0..99 as a ramp: the 2nd and 98th percentiles map to 0 and 1, the ends clamp.
    std::vector<float> ramp(100);
    for (std::size_t i = 0; i < ramp.size(); ++i) {
        ramp[i] = static_cast<float>(i);
    }
    const std::vector<float> n = normalisedDepth(ramp);
    CHECK(n.front() == 0.0f);
    CHECK(n.back() == 1.0f);
    CHECK(n[50] == Approx(0.5).margin(0.02));
    // One stray pixel does not flatten the map.
    std::vector<float> outlier = ramp;
    outlier[10] = 1.0e6f;
    CHECK(normalisedDepth(outlier)[50] == Approx(n[50]).margin(0.05));
    // A flat map has no depth: the middle everywhere. Not-a-number counts as the lowest.
    CHECK(normalisedDepth(std::vector<float>(16, 3.0f)) == std::vector<float>(16, 0.5f));
    std::vector<float> withNan = ramp;
    withNan[3] = std::numeric_limits<float>::quiet_NaN();
    CHECK(normalisedDepth(withNan)[3] == 0.0f);
    CHECK(normalisedDepth({}).empty());
    // The stored map is a distance: the nearest (1) is 0, the farthest (0) is 255.
    const Picture grey = greyPicture({1.0f, 0.5f, 0.0f}, 3, 1);
    REQUIRE(grey.valid());
    const std::vector<std::uint8_t> distance = distanceMap8(grey);
    REQUIRE(distance.size() == 3);
    CHECK(distance[0] == 0);
    CHECK(std::abs(static_cast<int>(distance[1]) - 128) <= 1); // the middle of the range
    CHECK(distance[2] == 255);
    CHECK(distanceMap8(Picture{}).empty());
    // The working size: 518 on the shorter side, the other in proportion.
    int w = 0;
    int h = 0;
    depthWorkingSize(1920, 1080, w, h);
    CHECK(std::pair(w, h) == std::pair(921, 518));
    depthWorkingSize(1080, 1920, w, h);
    CHECK(std::pair(w, h) == std::pair(518, 921));
    depthWorkingSize(64, 36, w, h);
    CHECK(h == 518);
    CHECK(w == 921);
}

TEST_CASE("An interpolated picture is keyed by its exact source time in 1/64 of a frame", "[ai][video][side]") {
    const Ticks period = frameTicks(1);
    CHECK(interpolationKey(frameTicks(2), period) == 128);
    CHECK(interpolationKey(frameAndFraction(2, 1), period) == 128 + 16);
    CHECK(interpolationKey(frameAndFraction(2, 2), period) == 128 + 32);
    // Times a hair apart (a rounding of the clip's speed) share a key; a whole 64th apart do not.
    CHECK(interpolationKey(frameAndFraction(2, 1) + Ticks{period.value / 400}, period) == 128 + 16);
    CHECK(interpolationKey(frameAndFraction(2, 1) + Ticks{period.value / 32}, period) == 128 + 18);
    CHECK(interpolationKey(Ticks{12345}, Ticks{0}) == 12345); // no period known: the time itself
}

TEST_CASE("A depth chain runs the model at the working size and gives the map at that size", "[ai][video][side]") {
    FakeBackend backend;
    const auto made = processPicture(pattern(64, 36), aiDepthChain(), backend, nullptr, {});
    REQUIRE(made);
    REQUIRE(backend.calls.size() == 1);
    CHECK(backend.calls[0].model == Model::Depth);
    CHECK(backend.calls[0].width == 921);
    CHECK(backend.calls[0].height == 518);
    CHECK(made->width == 921);
    CHECK(made->height == 518);
    CHECK(made->rgb[0] == 0);                                 // the fake's nearness ramps left to right
    CHECK(made->rgb[3 * 920] == 65535);
    // Alpha is not a depth: a picture with an alpha plane gives a map without one.
    FakeBackend second;
    const auto withAlpha = processPicture(pattern(64, 36, 0, true), aiDepthChain(), second, nullptr, {});
    REQUIRE(withAlpha);
    CHECK_FALSE(withAlpha->hasAlpha());
}

TEST_CASE("A depth job stores one 8-bit distance plane per frame and skips what the cache has", "[ai][video][side]") {
    ProcessedCache cache(freshDir("side-depth"));
    FakeBackend backend;
    FakeSource source(3, 64, 36);
    Runner runner(source, backend, cache);
    const Job job = depthJob(3);
    const auto first = runner.run(job, {}, {});
    REQUIRE(first);
    CHECK(first->processed == 3);
    CHECK(backend.calls.size() == 3);
    CHECK(cache.frames(job.identity).size() == 3);
    const auto stored = cache.get(job.identity, frameTicks(1), Ticks{frameTicks(1).value / 2});
    REQUIRE(stored);
    REQUIRE(stored->planes.size() == 1);
    CHECK(stored->planes[0].bytesPerTexel == 1);
    CHECK(stored->planes[0].width == 921);
    CHECK(stored->planes[0].height == 518);
    CHECK(stored->planes[0].data.front() == 255);  // the left edge is the farthest in the fake's ramp
    CHECK(stored->planes[0].data[920] == 0);       // the right edge the nearest
    // Run again: nothing is made.
    const auto again = runner.run(job, {}, {});
    REQUIRE(again);
    CHECK(again->processed == 0);
    CHECK(again->skipped == 3);
    CHECK(backend.calls.size() == 3);
}

TEST_CASE("Interpolation makes the picture between two frames at each exact time, and a stub for a time on a frame", "[ai][video][side]") {
    ProcessedCache cache(freshDir("side-interpolation"));
    FakeBackend backend;
    FakeSource source(10, 16, 12);
    Runner runner(source, backend, cache);
    const std::vector<Ticks> times{frameAndFraction(2, 1), frameAndFraction(2, 2), frameAndFraction(2, 3), frameTicks(3)};
    const Job job = interpolationJob(times);
    int reports = 0;
    const auto result = runner.run(job, [&](const Progress&) { ++reports; }, {});
    REQUIRE(result);
    CHECK(result->processed == 4);
    CHECK(reports >= 4);
    // The model ran for the three times between frames 2 and 3, at their weights; the time on frame 3 needed none.
    REQUIRE(backend.interpolations.size() == 3);
    CHECK(backend.interpolations[0].weight == Approx(0.25).margin(1e-3));
    CHECK(backend.interpolations[1].weight == Approx(0.5).margin(1e-3));
    CHECK(backend.interpolations[2].weight == Approx(0.75).margin(1e-3));
    CHECK(backend.interpolations[0].width == 16);
    CHECK(source.pictureCalls == 2); // frames 2 and 3, each converted once for all three times
    // The cache: one entry per time, under its key (the time in 1/64 frames); the picture is the cross-fade of the two frames.
    for (const std::int64_t key : {144, 160, 176, 192}) {
        INFO("key " << key);
        CHECK(cache.contains(job.identity, Ticks{key}, Ticks{0}));
    }
    const auto quarter = cache.get(job.identity, Ticks{144}, Ticks{0});
    REQUIRE(quarter);
    CHECK(quarter->width == 16);
    const Picture a = pattern(16, 12, 2);
    const Picture b = pattern(16, 12, 3);
    const int expected = static_cast<int>(std::lround(a.rgb[0] + (static_cast<double>(b.rgb[0]) - a.rgb[0]) * 0.25)) >> 8;
    CHECK(static_cast<int>(quarter->planes[0].data[0]) == expected);
    const auto stub = cache.get(job.identity, Ticks{192}, Ticks{0});
    REQUIRE(stub);
    CHECK(stub->width == 1); // a time on a frame: nothing to make, the decoded frame is shown
    // Again: all skipped, the model does not run.
    const auto again = runner.run(job, {}, {});
    REQUIRE(again);
    CHECK(again->processed == 0);
    CHECK(again->skipped == 4);
    CHECK(backend.interpolations.size() == 3);
}

TEST_CASE("An interpolation run can be cancelled, and a failing model ends it with the reason and nothing half stored", "[ai][video][side]") {
    {
        ProcessedCache cache(freshDir("side-interpolation-cancel"));
        FakeBackend backend;
        FakeSource source(10, 16, 12);
        Runner runner(source, backend, cache);
        std::stop_source stop;
        stop.request_stop();
        const auto cancelled = runner.run(interpolationJob({frameAndFraction(2, 2)}), {}, stop.get_token());
        REQUIRE(cancelled);
        CHECK(cancelled->cancelled);
        CHECK(cancelled->processed == 0);
        CHECK(backend.interpolations.empty());
    }
    {
        ProcessedCache cache(freshDir("side-interpolation-fail"));
        FakeBackend backend;
        backend.failInterpolationAt = 1;
        FakeSource source(10, 16, 12);
        Runner runner(source, backend, cache);
        const Job job = interpolationJob({frameAndFraction(2, 1), frameAndFraction(2, 2), frameAndFraction(2, 3)});
        const auto failed = runner.run(job, {}, {});
        REQUIRE_FALSE(failed);
        CHECK(failed.error().message.find("helper") != std::string::npos);
        CHECK(cache.contains(job.identity, Ticks{144}, Ticks{0}));        // the first was made and stays
        CHECK_FALSE(cache.contains(job.identity, Ticks{160}, Ticks{0}));  // the failing one left nothing
        // The model back: the run continues where it stopped.
        backend.failInterpolationAt = -1;
        const auto resumed = runner.run(job, {}, {});
        REQUIRE(resumed);
        CHECK(resumed->skipped == 1);
        CHECK(resumed->processed == 2);
    }
    {
        ProcessedCache cache(freshDir("side-interpolation-none"));
        FakeBackend backend;
        FakeSource source(10, 16, 12);
        Runner runner(source, backend, cache);
        Job empty = interpolationJob({});
        CHECK_FALSE(runner.run(empty, {}, {}));
        Job noPeriod = interpolationJob({frameTicks(1)});
        noPeriod.framePeriod = Ticks{};
        CHECK_FALSE(runner.run(noPeriod, {}, {}));
    }
}

TEST_CASE("A retimed clip with AI Frame Interpolation has one key per output frame; other clips have no such target", "[ai][video][side][targets]") {
    ProjectState s;
    auto media = mediaOf(s, 960, 540);
    s.media = {media};
    Clip clip;
    clip.id = s.ids.allocate<ClipId>();
    clip.media = media->id;
    clip.timelineRange = {Ticks::fromSeconds(4), Ticks::fromSeconds(2)};
    clip.sourceIn = Ticks::fromSeconds(1);
    clip.speed = Speed{1, 2}; // half speed: every other output frame falls between two frames
    {
        ClipContent content = clip.content();
        content.timeInterpolation = TimeInterpolation::AiInterpolation;
        clip.setContent(std::move(content));
    }
    const Sequence seq = sequenceOf(s, {clip});
    const Clip& placed = seq.videoTracks.front()->clips.front();
    const auto t = interpolationTarget(s, seq, placed, 0);
    REQUIRE(t.has_value());
    CHECK(t->kind == JobKind::Interpolation);
    CHECK(t->times.size() == 50); // 2 s at 25 fps
    CHECK(t->keys.size() == 50);
    CHECK(t->expected == 50);
    CHECK(t->framePeriod == framesToTicks(1, Rate{25, 1}));
    CHECK(t->keys[0] == Ticks{25 * 64});       // the source In: second 1 is frame 25
    CHECK(t->keys[1] == Ticks{25 * 64 + 32});  // half a frame later
    CHECK(t->identity.valid());
    CHECK(aiChainLabel(t->chain) == "Frame interpolation");
    CHECK(t->identity == identityOf(*media, 0, aiInterpolationChain()));
    // Its keys are what a cache is asked about: coveredCount counts those stored.
    ProcessedCache cache(freshDir("side-targets"));
    CHECK(coveredCount(cache, *t) == 0);
    for (std::size_t i = 0; i < 10; ++i) {
        StoredFrame stored = packRgba8(pattern(4, 4));
        stored.pts = t->keys[i].value;
        REQUIRE(cache.put(t->identity, stored));
    }
    CHECK(coveredCount(cache, *t) == 10);

    // The sequence lists it among the targets.
    const auto all = sequenceTargets(s, seq, std::nullopt);
    REQUIRE(all.size() == 1);
    CHECK(all[0].kind == JobKind::Interpolation);

    // At the media's own speed nothing falls between frames; another mode is no AI interpolation.
    Clip unchanged = placed;
    unchanged.speed = Speed{1, 1};
    CHECK_FALSE(interpolationTarget(s, seq, unchanged, 0).has_value());
    Clip flow = placed;
    {
        ClipContent content = flow.content();
        content.timeInterpolation = TimeInterpolation::OpticalFlow;
        flow.setContent(std::move(content));
    }
    CHECK_FALSE(interpolationTarget(s, seq, flow, 0).has_value());
    CHECK(sequenceTargets(s, sequenceOf(s, {flow}), std::nullopt).empty());
}

TEST_CASE("A clip with AI Refocus has a depth target shared by every Refocus; a disabled one has none", "[ai][video][side][targets]") {
    ProjectState s;
    auto media = mediaOf(s, 960, 540);
    s.media = {media};
    const auto refocus = [&](bool enabled) {
        EffectInstance e;
        e.id = s.ids.allocate<EffectInstanceId>();
        e.effect = std::string(kAiRefocusEffect);
        e.enabled = enabled;
        return e;
    };
    Clip clip;
    clip.id = s.ids.allocate<ClipId>();
    clip.media = media->id;
    clip.timelineRange = {Ticks{}, Ticks::fromSeconds(2)};
    clip.setEffects({refocus(true), refocus(true)});
    const Sequence seq = sequenceOf(s, {clip});
    const Clip& placed = seq.videoTracks.front()->clips.front();
    const auto t = depthTarget(s, seq, placed, 0);
    REQUIRE(t.has_value());
    CHECK(t->kind == JobKind::Depth);
    CHECK(t->chain.stages.size() == 1); // two Refocus effects, one depth map
    CHECK(t->expected == 50);
    CHECK(t->identity == identityOf(*media, 0, aiDepthChain()));
    CHECK(t->identity != identityOf(*media, 0, aiInterpolationChain()));
    // Restore and Denoise ignore it: it is not a source-stage effect.
    const auto pictures = clipTarget(s, seq, placed, 0);
    REQUIRE(pictures.has_value());
    CHECK(pictures->chain.empty());
    CHECK(pictures->kind == JobKind::Pictures);
    // A disabled Refocus makes no depth maps.
    Clip off = placed;
    off.setEffects({refocus(false)});
    CHECK_FALSE(depthTarget(s, seq, off, 0).has_value());
    // The sequence lists the depth target (and only it).
    const auto all = sequenceTargets(s, seq, std::nullopt);
    REQUIRE(all.size() == 1);
    CHECK(all[0].kind == JobKind::Depth);
}
