// AI-14 SAM 2.1 out of process: the segment_frame protocol (round trips and bounds), and HelperSegmenter against the
// scripted wabcut-embed (fake_embed: the object is the red pixels): a prompt and tracked frames, one request per frame
// through a whole run, a GPU crash while loading (the CPU takes over), a crash mid-run (restart, the prompt replayed),
// a helper that keeps crashing, a hung helper (timeout), cancel, and release.

#include "SegmentTestUtil.h"

#include <wabcut/ai/search/EmbedProtocol.h>
#include <wabcut/ai/segment/HelperSegmenter.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

using namespace wabcut;
using namespace wabcut::ai;
using namespace wabcut::ai::segment;
using namespace wabcut::ai::segment::test;
namespace embed = wabcut::ai::search::embed;

namespace {

HelperSegmenterOptions fakeOptions(const std::string& mode, const std::string& dirName) {
    HelperSegmenterOptions o;
    o.executable = WABCUT_FAKE_EMBED;
    o.arguments = {"--mode", mode};
    o.modelDirectory = freshDir(dirName + "-model");
    o.modelVersion = "fake-sam";
    o.workDir = freshDir(dirName);
    o.device = "auto";
    o.startTimeout = std::chrono::seconds(20);
    o.requestTimeout = std::chrono::seconds(20);
    return o;
}

search::Image pictureOf(const ScriptedVideo& video, int frame) { return video.draw(frame, 1024, 1024); }

Prompt clickAt(double x, double y) {
    Prompt p;
    p.points = {PromptPoint{x, y, true}};
    return p;
}

} // namespace

TEST_CASE("The segment_frame protocol: round trips and bounds", "[ai][segment][protocol]") {
    embed::SegmentFrame r;
    r.id = "7";
    r.mode = "track";
    r.images = embed::ImageBatch{"C:/tmp/p.rgb", 1024, 1024, 1};
    r.frame = 3;
    r.frames = 10;
    r.points = {{10.0f, 20.0f, 1}, {30.0f, 40.0f, 0}, {1.0f, 2.0f, 2}, {500.0f, 600.0f, 3}};
    r.output = "C:/tmp/m.a8";
    r.width = 512;
    r.height = 288;
    auto back = embed::decodeRequest(embed::encode(embed::Request{r}));
    REQUIRE(back);
    const auto* seg = std::get_if<embed::SegmentFrame>(&*back);
    REQUIRE(seg);
    CHECK(seg->mode == "track");
    CHECK(seg->frame == 3);
    CHECK(seg->frames == 10);
    REQUIRE(seg->points.size() == 4);
    CHECK(seg->points[3].label == 3);
    CHECK(seg->points[1].x == 30.0f);
    CHECK(seg->width == 512);

    const auto refused = [](embed::SegmentFrame s) { return !embed::decodeRequest(embed::encode(embed::Request{s})).has_value(); };
    embed::SegmentFrame bad = r;
    bad.mode = "guess";
    CHECK(refused(bad));
    bad = r;
    bad.width = embed::kMaxMatteSide + 1;
    CHECK(refused(bad));
    bad = r;
    bad.images.count = 2;
    CHECK(refused(bad));
    bad = r;
    bad.frame = 10; // past the run
    CHECK(refused(bad));
    bad = r;
    bad.points.assign(static_cast<std::size_t>(embed::kMaxSegmentPoints) + 1, embed::SegmentPoint{});
    CHECK(refused(bad));

    embed::Segmented s{"7", 0.93f, 8.5f, true, 512, 288, 41.0, "DirectML"};
    auto reply = embed::decodeResponse(embed::encode(embed::Response{s}));
    REQUIRE(reply);
    const auto* got = std::get_if<embed::Segmented>(&*reply);
    REQUIRE(got);
    CHECK(got->iou == 0.93f);
    CHECK(got->objectScore == 8.5f);
    CHECK(got->present);
    CHECK(got->device == "DirectML");

    embed::Load load;
    load.id = "1";
    load.segmentation = "C:/models/sam2.1-tiny-video";
    auto loaded = embed::decodeRequest(embed::encode(embed::Request{load}));
    REQUIRE(loaded);
    CHECK(std::get<embed::Load>(*loaded).segmentation == load.segmentation);

    embed::Detect detect;
    detect.id = "2";
    detect.images = embed::ImageBatch{"C:/tmp/d.rgb", 64, 64, 1};
    detect.allClasses = true;
    auto detected = embed::decodeRequest(embed::encode(embed::Request{detect}));
    REQUIRE(detected);
    CHECK(std::get<embed::Detect>(*detected).allClasses);
}

TEST_CASE("HelperSegmenter: a prompt and tracked frames through wabcut-embed", "[ai][segment][helper]") {
    HelperSegmenter model(fakeOptions("normal", "helper-normal"));
    const ScriptedVideo video(ScriptedScene{100, -1});
    CHECK(model.inputSize() == 1024);
    CHECK(model.modelVersion() == "fake-sam");

    SECTION("the matte is the object, at the requested size") {
        auto first = model.prompt(pictureOf(video, 10), clickAt(0.2, 0.4), 0, 5, 512, 288, {});
        REQUIRE(first);
        CHECK(first->present);
        CHECK(first->matte.width == 512);
        CHECK(first->device == "FakeCPU");
        const MatteImage truth = RedSegmenter::segment(pictureOf(video, 10), 512, 288).matte;
        CHECK(iouOf(first->matte, truth) > 0.98);
        for (int i = 1; i < 5; ++i) {
            auto next = model.track(pictureOf(video, 10 + i), i, 5, 512, 288, {});
            REQUIRE(next);
            CHECK(iouOf(next->matte, RedSegmenter::segment(pictureOf(video, 10 + i), 512, 288).matte) > 0.98);
        }
        CHECK(model.starts() == 1);
        CHECK_FALSE(model.device().empty());
        // The picture and matte files are gone.
        CHECK(std::filesystem::is_empty(std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "ai_segment" / "helper-normal"));
    }
    SECTION("refused before the helper is asked: a track before a prompt, an empty prompt, a wrong picture size") {
        CHECK(model.track(pictureOf(video, 1), 1, 2, 64, 36, {}).error().code == ErrorCode::InvalidArgument);
        CHECK(model.prompt(pictureOf(video, 1), Prompt{}, 0, 2, 64, 36, {}).error().code == ErrorCode::InvalidArgument);
        CHECK(model.prompt(video.draw(1, 512, 512), clickAt(0.2, 0.4), 0, 2, 64, 36, {}).error().code == ErrorCode::InvalidArgument);
        CHECK(model.starts() == 0);
    }
    SECTION("a whole run is one request per frame") {
        PropagationOptions o;
        o.prompt = clickAt(0.15, 0.4);
        o.prompt.time = frameTicks(5);
        o.span = TimeRange{Ticks{}, frameTicks(30)};
        ScriptedVideo frames(ScriptedScene{100, -1});
        auto r = propagate(model, frames, o);
        REQUIRE(r);
        CHECK(r->track.frames.size() == 30);
        CHECK(r->lost == 0);
        CHECK(frames.requests == 31); // the prompt frame twice (forward and backward pass)
        CHECK(model.starts() == 1);
    }
    SECTION("release() frees the model; the next frame replays the prompt into a new helper") {
        REQUIRE(model.prompt(pictureOf(video, 10), clickAt(0.2, 0.4), 0, 5, 64, 36, {}));
        model.release();
        auto next = model.track(pictureOf(video, 11), 1, 5, 64, 36, {});
        REQUIRE(next);
        CHECK(next->present);
        CHECK(model.starts() == 2);
    }
}

TEST_CASE("HelperSegmenter survives a crashing helper, gives up on one that keeps crashing", "[ai][segment][helper]") {
    const ScriptedVideo video(ScriptedScene{100, -1});
    SECTION("the GPU provider crashing at load: once more on the CPU") {
        HelperSegmenter model(fakeOptions("crash-gpu-segmentation", "helper-gpu"));
        auto r = model.prompt(pictureOf(video, 3), clickAt(0.12, 0.4), 0, 2, 64, 36, {});
        REQUIRE(r);
        CHECK(r->present);
        CHECK(model.starts() == 2);
        REQUIRE(model.track(pictureOf(video, 4), 1, 2, 64, 36, {}));
        CHECK(model.starts() == 2); // the CPU sticks
    }
    SECTION("a crash mid-run: the helper restarts, the prompt is replayed, the run goes on") {
        HelperSegmenterOptions o = fakeOptions("crash-on-track", "helper-crash");
        HelperSegmenter model(o);
        REQUIRE(model.prompt(pictureOf(video, 0), clickAt(0.12, 0.4), 0, 7, 64, 36, {}));
        for (int i = 1; i <= 6; ++i) {
            auto r = model.track(pictureOf(video, i), i, 7, 64, 36, {});
            INFO("frame " << i);
            REQUIRE(r);
            CHECK(r->present);
        }
        CHECK(model.starts() == 3); // crashed at tracked frames 3 and 5
    }
    SECTION("a helper that keeps crashing ends the run with an error") {
        HelperSegmenterOptions o = fakeOptions("crash-on-track", "helper-crashes");
        o.maxRestarts = 1;
        HelperSegmenter model(o);
        REQUIRE(model.prompt(pictureOf(video, 0), clickAt(0.12, 0.4), 0, 9, 64, 36, {}));
        std::optional<Error> failed;
        for (int i = 1; i <= 8 && !failed; ++i) {
            if (auto r = model.track(pictureOf(video, i), i, 9, 64, 36, {}); !r) {
                failed = r.error();
            }
        }
        REQUIRE(failed);
        CHECK(failed->message.find("keeps crashing") != std::string::npos);
    }
}

TEST_CASE("HelperSegmenter: a hung helper times out; a stop request cancels", "[ai][segment][helper]") {
    const ScriptedVideo video(ScriptedScene{100, -1});
    SECTION("timeout") {
        HelperSegmenterOptions o = fakeOptions("hang-on-embed", "helper-hang");
        o.requestTimeout = std::chrono::milliseconds(400);
        o.maxRestarts = 0;
        HelperSegmenter model(o);
        const auto started = std::chrono::steady_clock::now();
        auto r = model.prompt(pictureOf(video, 0), clickAt(0.12, 0.4), 0, 2, 64, 36, {});
        REQUIRE_FALSE(r);
        CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(10));
    }
    SECTION("cancel") {
        HelperSegmenter model(fakeOptions("hang-on-embed", "helper-cancel"));
        std::stop_source stop;
        std::jthread canceller([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            stop.request_stop();
        });
        const auto started = std::chrono::steady_clock::now();
        auto r = model.prompt(pictureOf(video, 0), clickAt(0.12, 0.4), 0, 2, 64, 36, stop.get_token());
        REQUIRE_FALSE(r);
        CHECK(r.error().code == ErrorCode::Cancelled);
        CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(10));
    }
}

TEST_CASE("The SAM 2.1 model files: Apache-2.0, pinned, about 190 MB", "[ai][segment][helper]") {
    const auto files = segmentModelFiles();
    REQUIRE(files.size() == 6);
    std::uint64_t total = 0;
    for (const ModelInfo* m : files) {
        REQUIRE(m != nullptr);
        CHECK(m->license == "Apache-2.0");
        CHECK(m->kind == ModelKind::Segmentation);
        CHECK(m->sha256.size() == 64);
        CHECK(m->url.find("3b2984dd865f6e9d2cc6aed0be6a5a5c2eb352ce") != std::string::npos);
        CHECK(m->fileName.starts_with("sam2.1-tiny-video/"));
        CHECK(m->licenseCard.find("Apache License 2.0") != std::string::npos);
        total += m->size;
    }
    CHECK(total > 185'000'000);
    CHECK(total < 195'000'000);
    CHECK(segmentModelVersion() == "sam2.1-tiny-video-aa7a8542-0461896d-v1");
    const ModelStore store(freshDir("store-root"));
    CHECK(segmentModelDirectory(store) == store.root() / "sam2.1-tiny-video");
    CHECK_FALSE(segmentModelInstalled(store));
}
