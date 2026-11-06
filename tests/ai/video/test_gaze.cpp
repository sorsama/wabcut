// AI Eye Contact (card 72) on the AI video runner: the landmark network is a verified, pinned download (the detector comes with it); the
// rolled crop round a face, the network's input and its landmarks back in the picture's pixels; the table of eyelid / iris / nose / cheek
// landmarks a helper request answers in; the eyes' measurements from those landmarks (corners, lids, iris, the face's turn) and their
// stored form; a gaze job per frame at the working size with the fake backend; a clip's gaze target; HelperBackend through the scripted
// helper. The opt-in test at the end runs the real models on a photo of a face (WABCUT_TEST_VIDEO_MODELS_DIR, WABCUT_TEST_FACE_IMAGE).

#include "VideoTestUtil.h"

#include <wabcut/ai/ModelStore.h>
#include <wabcut/ai/video/Gaze.h>
#include <wabcut/ai/video/HelperBackend.h>
#include <wabcut/ai/video/ModelKeys.h>
#include <wabcut/ai/video/ProcessedCache.h>
#include <wabcut/ai/video/Runner.h>
#include <wabcut/ai/video/Targets.h>
#include <wabcut/ai/video/VideoModels.h>
#include <wabcut/model/Clip.h>
#include <wabcut/model/EyeContact.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/model/Sequence.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <numbers>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::ai;
using namespace wabcut::ai::video;
using namespace wabcut::ai::video::test;
using Catch::Approx;

namespace {

Job gazeJob(int frames) {
    Job job;
    job.kind = JobKind::Gaze;
    job.chain = aiGazeChain();
    job.identity = identity(aiChainHash(job.chain));
    job.range = TimeRange{Ticks{}, frameTicks(frames)};
    job.expectedFrames = frames;
    return job;
}

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

TEST_CASE("The landmark model is a verified pinned download and brings the face detector", "[ai][video][gaze][models]") {
    const ModelInfo* m = findModel("video-face-landmarks");
    REQUIRE(m != nullptr);
    CHECK(m->kind == ModelKind::Video);
    CHECK(m->license == "Apache-2.0");
    CHECK(isAllowedModelLicense(m->license));
    CHECK_FALSE(requiresLicenseOptIn(*m));
    CHECK(m->size == 4'920'995);
    CHECK(m->sha256 == "f38c3321ceffbc9e95103480ad38cc3f52e7e1bde2bcee7cd9355d0b9138ac0c");
    // Pinned to a revision of the conversion's repository, never a branch; the digest pins the bytes either way.
    CHECK(m->url.starts_with("https://huggingface.co/naklitechie/face-landmarks-onnx/resolve/575c33816c840c5b156398adb485e4f8d138adc2/"));
    CHECK(m->fileName.starts_with("ai-video/"));
    CHECK(m->licenseCard.find("Apache License 2.0") != std::string::npos);
    CHECK(m->licenseCard.find("no picture leaves it") != std::string::npos);
    CHECK(m->licenseCard.find("https://") != std::string::npos);
    CHECK(describe(Model::FaceLandmarks).key == kKeyFaceLandmarks);
    CHECK(describe(Model::FaceLandmarks).scale == 1);
    // One stage, one step; both models in the chain, the detector first, both in its cache's version.
    const AiChain chain = aiGazeChain();
    REQUIRE(stepsOf(chain).size() == 1);
    CHECK(stepsOf(chain).front().model == Model::FaceLandmarks);
    const std::vector<Model> models = modelsOf(chain);
    REQUIRE(models.size() == 2);
    CHECK(models[0] == Model::FaceDetect);
    CHECK(models[1] == Model::FaceLandmarks);
    CHECK(modelVersion(chain) == "yunet-face-detect-8f2383e4+mediapipe-face-landmarks-f38c3321-p1");
    CHECK(aiChainLabel(chain) == "Eye measurements");
    CHECK(chain.stages.front().isSideProduct());
    CHECK_FALSE(chain.stages.front().isNoop());
    ModelStore empty(freshDir("gaze-store"));
    CHECK_FALSE(modelsInstalled(chain, empty));
    CHECK(missingBytes(chain, empty) == m->size + findModel("yunet-2023mar")->size);
    const std::string card = downloadCard(chain, empty);
    CHECK(card.find("YuNet") != std::string::npos);
    CHECK(card.find("MediaPipe") != std::string::npos);
    CHECK(missingModelsText(chain, empty).find("Face landmarks (5 MB)") != std::string::npos);
    // The estimate counts the picture at the working size, like the face maps.
    CHECK(estimateStepMilliseconds(stepsOf(chain).front(), 3840, 2160, true) == Approx(estimateStepMilliseconds(stepsOf(chain).front(), 1280, 720, true)));
}

TEST_CASE("The landmark crop is the face box 1.5 times larger, rolled by the line between the eyes", "[ai][video][gaze]") {
    const GazeCrop crop = gazeCropFor(FaceBox{100.0, 50.0, 200.0, 240.0}, {150.0, 120.0}, {250.0, 100.0});
    CHECK(crop.side == Approx(360.0));
    CHECK(crop.cx == Approx(200.0));
    CHECK(crop.cy == Approx(170.0));
    CHECK(crop.angle == Approx(std::atan2(-20.0, 100.0)));
    CHECK(gazeCropFor(FaceBox{0.0, 0.0, 20.0, 20.0}, {5.0, 8.0}, {15.0, 8.0}).side == Approx(96.0)); // a floor, like the parser's
    CHECK(gazeCropFor(FaceBox{0.0, 0.0, 20.0, 20.0}, {5.0, 8.0}, {15.0, 8.0}).angle == Approx(0.0));
}

TEST_CASE("The network's input is the crop sampled through its roll as 256 x 256 x 3 NHWC floats", "[ai][video][gaze]") {
    // A picture whose red is x / 199 and green y / 199.
    Picture p;
    p.width = 200;
    p.height = 200;
    p.rgb.resize(p.pixels() * 3);
    for (int y = 0; y < 200; ++y) {
        for (int x = 0; x < 200; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * 200 + static_cast<std::size_t>(x)) * 3;
            p.rgb[i] = static_cast<std::uint16_t>(65535.0 * x / 199.0);
            p.rgb[i + 1] = static_cast<std::uint16_t>(65535.0 * y / 199.0);
            p.rgb[i + 2] = 0;
        }
    }
    const auto at = [](const std::vector<float>& in, int u, int v, int channel) {
        return in[(static_cast<std::size_t>(v) * kGazeCropSize + static_cast<std::size_t>(u)) * 3 + static_cast<std::size_t>(channel)];
    };
    // Unrolled, one crop pixel per picture pixel: the crop's middle is the picture point (100, 100); outside the picture the edge repeats.
    const std::vector<float> flat = gazeInput(p, GazeCrop{100.0, 100.0, 256.0, 0.0});
    REQUIRE(flat.size() == 256u * 256u * 3u);
    CHECK(at(flat, 128, 128, 0) == Approx(100.0 / 199.0).margin(1e-4));
    CHECK(at(flat, 128, 128, 1) == Approx(100.0 / 199.0).margin(1e-4));
    CHECK(at(flat, 138, 118, 0) == Approx(110.0 / 199.0).margin(1e-4));
    CHECK(at(flat, 138, 118, 1) == Approx(90.0 / 199.0).margin(1e-4));
    CHECK(at(flat, 0, 0, 0) == Approx(0.0).margin(1e-6));      // 28 pixels left of the picture: the edge
    CHECK(at(flat, 255, 255, 1) == Approx(1.0).margin(1e-6));  // beyond the bottom edge
    CHECK(at(flat, 128, 128, 2) == 0.0f);
    // Rolled by a quarter turn: the crop's x runs down the picture and its y runs back to the left.
    const std::vector<float> rolled = gazeInput(p, GazeCrop{100.0, 100.0, 256.0, std::numbers::pi / 2.0});
    CHECK(at(rolled, 138, 128, 1) == Approx(110.0 / 199.0).margin(1e-3)); // u + 10: ten pixels down
    CHECK(at(rolled, 138, 128, 0) == Approx(100.0 / 199.0).margin(1e-3));
    CHECK(at(rolled, 128, 138, 0) == Approx(90.0 / 199.0).margin(1e-3));  // v + 10: ten pixels left
    // A smaller window than the network's input enlarges; an invalid picture or window gives nothing.
    const std::vector<float> zoomed = gazeInput(p, GazeCrop{100.0, 100.0, 128.0, 0.0});
    CHECK(at(zoomed, 138, 128, 0) == Approx((100.0 + 10.0 * 0.5) / 199.0).margin(1e-3));
    CHECK(gazeInput(Picture{}, GazeCrop{100.0, 100.0, 256.0, 0.0}).empty());
    CHECK(gazeInput(p, GazeCrop{100.0, 100.0, 0.0, 0.0}).empty());
}

TEST_CASE("The network's landmarks come back in the picture's pixels through the crop", "[ai][video][gaze]") {
    std::vector<float> output(static_cast<std::size_t>(kGazeLandmarkCount) * 3);
    for (int i = 0; i < kGazeLandmarkCount; ++i) {
        output[static_cast<std::size_t>(3 * i)] = static_cast<float>(128 + i % 10);
        output[static_cast<std::size_t>(3 * i + 1)] = static_cast<float>(128 - i % 7);
        output[static_cast<std::size_t>(3 * i + 2)] = 5.0f;
    }
    // Unrolled, two picture pixels per crop pixel: (u - 128) * 2 from the crop's centre.
    const auto flat = gazeLandmarksOf(output, GazeCrop{300.0, 200.0, 512.0, 0.0});
    REQUIRE(flat.size() == 478u);
    CHECK(flat[13].x == Approx(300.0 + 2.0 * 3.0));  // 13 % 10 = 3
    CHECK(flat[13].y == Approx(200.0 - 2.0 * 6.0));  // 13 % 7 = 6
    CHECK(flat[13].z == Approx(10.0));
    // Rolled by a quarter turn: the offsets turn with it (x' = cx - ly, y' = cy + lx).
    const auto rolled = gazeLandmarksOf(output, GazeCrop{300.0, 200.0, 512.0, std::numbers::pi / 2.0});
    CHECK(rolled[13].x == Approx(300.0 + 2.0 * 6.0));
    CHECK(rolled[13].y == Approx(200.0 + 2.0 * 3.0));
    // An output of the wrong size is nothing.
    CHECK(gazeLandmarksOf(std::span<const float>(output).first(100), GazeCrop{300.0, 200.0, 512.0, 0.0}).empty());
    // The landmarks the table keeps are the eyelid contours, the irises, the nose tip and the cheeks of MediaPipe's mesh.
    const std::span<const int> indices = gazeLandmarkIndices();
    REQUIRE(indices.size() == static_cast<std::size_t>(kGazeLandmarkSlots));
    CHECK(indices[0] == 33);
    CHECK(indices[8] == 133);
    CHECK(indices[16] == 362);
    CHECK(indices[24] == 263);
    CHECK(indices[32] == 468);
    CHECK(indices[37] == 473);
    CHECK(indices[42] == 1);
    CHECK(indices[43] == 234);
    CHECK(indices[44] == 454);
    CHECK(*std::ranges::max_element(indices) < kGazeLandmarkCount);
    std::set<int> distinct(indices.begin(), indices.end());
    CHECK(distinct.size() == indices.size());
}

TEST_CASE("The landmark table round trips every landmark, the box, the presence and the working size", "[ai][video][gaze]") {
    const GazeLandmarkFace face = fakeLandmarkFace(1280, 720, FakeFaceSpec{0.12, 0.03, 8.0});
    const Picture table = gazeTablePicture(std::span(&face, 1), 1280, 720);
    CHECK(table.width == kGazeSlotCount);
    CHECK(table.height == 1);
    const auto parsed = gazeTableFromPicture(table);
    REQUIRE(parsed);
    CHECK(parsed->workWidth == 1280);
    CHECK(parsed->workHeight == 720);
    REQUIRE(parsed->faces.size() == 1);
    const GazeLandmarkFace& back = parsed->faces.front();
    REQUIRE(back.points.size() == face.points.size());
    for (std::size_t i = 0; i < face.points.size(); ++i) {
        INFO("landmark " << i);
        CHECK(back.points[i].x == Approx(face.points[i].x).margin(0.1)); // 16 bits over twice the picture: 0.04 px at 1280
        CHECK(back.points[i].y == Approx(face.points[i].y).margin(0.1));
    }
    CHECK(back.box.x == Approx(face.box.x).margin(0.1));
    CHECK(back.box.h == Approx(face.box.h).margin(0.1));
    CHECK(back.presence == Approx(0.99).margin(1e-4));
    // Landmarks a little outside the picture survive (a face at its edge).
    GazeLandmarkFace edge = face;
    edge.points[3].x = -40.0;
    edge.points[4].x = 1280.0 + 60.0;
    const Picture edgeTable = gazeTablePicture(std::span(&edge, 1), 1280, 720);
    const auto edgeBack = gazeTableFromPicture(edgeTable);
    REQUIRE(edgeBack);
    CHECK(edgeBack->faces.front().points[3].x == Approx(-40.0).margin(0.1));
    CHECK(edgeBack->faces.front().points[4].x == Approx(1340.0).margin(0.1));
    // No face: one row that says so, and still the working size; two faces: two rows.
    const Picture none = gazeTablePicture({}, 640, 360);
    const auto noneBack = gazeTableFromPicture(none);
    REQUIRE(noneBack);
    CHECK(noneBack->faces.empty());
    CHECK(noneBack->workWidth == 640);
    CHECK(noneBack->workHeight == 360);
    const auto two = gazeTableFromPicture(fakeGazeTable(640, 360, {}, 2));
    REQUIRE(two);
    CHECK(two->faces.size() == 2);
    // A picture that is not a table is refused.
    CHECK_FALSE(gazeTableFromPicture(Picture{}).has_value());
    CHECK_FALSE(gazeTableFromPicture(pattern(10, 10)).has_value());
    Picture noSize = table;
    noSize.rgb[3 * 47 + 1] = 0;
    CHECK_FALSE(gazeTableFromPicture(noSize).has_value());
}

TEST_CASE("A face's measurements come from its landmarks: corners, lids, iris, turn and size", "[ai][video][gaze]") {
    const GazeFace level = gazeFaceOf(fakeLandmarkFace(1280, 720, FakeFaceSpec{0.12}), 1280, 720);
    // The eye on the picture's left: its middle at (0.40, 0.40) of the picture, 0.045 half wide, lids 0.012 above / 0.007 below the corner line.
    const GazeEye& left = level.eyes[0];
    CHECK(left.cx == Approx(0.40).margin(1e-9));
    CHECK(left.cy == Approx(0.40).margin(1e-9));
    CHECK(left.angle == Approx(0.0).margin(1e-9));
    CHECK(left.halfWidth == Approx(0.045).margin(1e-9));
    CHECK(left.upper == Approx(0.012).margin(1e-9));
    CHECK(left.lower == Approx(0.007).margin(1e-9));
    CHECK(left.irisX == Approx(0.0).margin(1e-9));
    CHECK(left.irisY == Approx(0.5 * (0.007 - 0.012) + 0.12 * 2.0 * 0.045).margin(1e-9)); // the middle of the opening, 12 % of the eye's width lower
    CHECK(left.irisRadius == Approx(0.018).margin(1e-9));
    CHECK(left.valid());
    CHECK(level.eyes[1].cx == Approx(0.60).margin(1e-9));
    CHECK(level.eyes[1].valid());
    CHECK(level.turn == Approx(0.0).margin(1e-9));
    CHECK(level.score == Approx(0.99));
    CHECK(level.size == Approx(0.44));
    // What the effect makes of it: the iris is 12 % of the eye's width low, in the range, to be moved up.
    const EyeCorrection c = correctEye(left, level.turn, EyeContactSettings{});
    CHECK(c.gazeY == Approx(0.12).margin(1e-6));
    CHECK(c.active);
    // The same face rolled by 12 degrees in the picture: the eyes' frames roll with it, so every number in them stays.
    const GazeFace rolled = gazeFaceOf(fakeLandmarkFace(1280, 720, FakeFaceSpec{0.12, 0.0, 12.0}), 1280, 720);
    for (int e = 0; e < 2; ++e) {
        CHECK(rolled.eyes[static_cast<std::size_t>(e)].angle == Approx(12.0 * std::numbers::pi / 180.0).margin(1e-6));
        CHECK(rolled.eyes[static_cast<std::size_t>(e)].halfWidth == Approx(0.045).margin(1e-6));
        CHECK(rolled.eyes[static_cast<std::size_t>(e)].upper == Approx(0.012).margin(1e-6));
        CHECK(rolled.eyes[static_cast<std::size_t>(e)].lower == Approx(0.007).margin(1e-6));
        CHECK(rolled.eyes[static_cast<std::size_t>(e)].irisX == Approx(0.0).margin(1e-6));
        CHECK(rolled.eyes[static_cast<std::size_t>(e)].irisY == Approx(left.irisY).margin(1e-6));
        CHECK(rolled.eyes[static_cast<std::size_t>(e)].irisRadius == Approx(0.018).margin(1e-6));
    }
    // An iris to the right of the middle is positive x in the eye's frame.
    const GazeFace right = gazeFaceOf(fakeLandmarkFace(1280, 720, FakeFaceSpec{0.0, 0.10}), 1280, 720);
    CHECK(right.eyes[0].irisX == Approx(0.10 * 2.0 * 0.045).margin(1e-9));
    // The face's turn is where the nose lies between the cheeks: 0 in the middle, 0.5 three quarters of the way to a cheek's side.
    CHECK(gazeFaceOf(fakeLandmarkFace(1280, 720, FakeFaceSpec{0.12, 0.0, 0.0, 0.012, 0.007, 0.5}), 1280, 720).turn == Approx(0.5).margin(1e-6));
    CHECK(gazeFaceOf(fakeLandmarkFace(1280, 720, FakeFaceSpec{0.12, 0.0, 0.0, 0.012, 0.007, 1.0}), 1280, 720).turn == Approx(1.0).margin(1e-6));
    // Landmarks that make no sense measure no valid eye: both corners in one place.
    GazeLandmarkFace broken = fakeLandmarkFace(1280, 720);
    broken.points[8] = broken.points[0];
    CHECK_FALSE(gazeFaceOf(broken, 1280, 720).eyes[0].valid());
    CHECK(gazeFaceOf(broken, 1280, 720).eyes[1].valid());
    CHECK_FALSE(gazeFaceOf(GazeLandmarkFace{}, 1280, 720).eyes[0].valid());
}

TEST_CASE("The measurements are stored per frame, largest face first, at most four, and read back", "[ai][video][gaze]") {
    // Two faces of different sizes, the smaller first in the table.
    GazeLandmarkFace small = fakeLandmarkFace(640, 360, FakeFaceSpec{0.05});
    small.box.w = 0.20 * 640.0;
    GazeLandmarkFace big = fakeLandmarkFace(640, 360, FakeFaceSpec{0.15});
    const std::vector<GazeLandmarkFace> faces{small, big};
    const auto stored = gazeStoredFrame(gazeTablePicture(faces, 640, 360), frameTicks(2), frameTicks(1));
    REQUIRE(stored);
    CHECK(stored->pts == frameTicks(2).value);
    CHECK(stored->duration == frameTicks(1).value);
    CHECK(stored->width == 640);
    CHECK(stored->height == 360);
    REQUIRE(stored->planes.size() == 1);
    CHECK(stored->planes[0].bytesPerTexel == 4);
    CHECK(stored->planes[0].height == 1);
    const std::vector<GazeFace> back = gazeFacesOfStored(*stored);
    REQUIRE(back.size() == 2);
    CHECK(back[0].size == Approx(0.44).margin(5e-5)); // the larger face first (the table keeps 16 bits over twice the picture)
    CHECK(back[1].size == Approx(0.20).margin(5e-5));
    const double bigIris = 0.5 * (0.007 - 0.012) + 0.15 * 2.0 * 0.045;
    CHECK(back[0].eyes[0].irisY == Approx(bigIris).margin(5e-5));
    CHECK(back[0].eyes[1].cx == Approx(0.60).margin(5e-5));
    // Through the cache's file format and back.
    const auto bytes = encodeFrame(*stored);
    REQUIRE(bytes);
    const auto decoded = decodeFrame(*bytes);
    REQUIRE(decoded);
    CHECK(gazeFacesOfStored(*decoded) == back);
    // More than four faces: the four largest are kept.
    const std::vector<GazeLandmarkFace> crowd(6, big);
    const auto crowded = gazeStoredFrame(gazeTablePicture(crowd, 640, 360), Ticks{}, frameTicks(1));
    REQUIRE(crowded);
    CHECK(gazeFacesOfStored(*crowded).size() == static_cast<std::size_t>(kGazeMaxFaces));
    // A frame with no face is a frame with measurements: none.
    const auto empty = gazeStoredFrame(gazeTablePicture({}, 640, 360), Ticks{}, frameTicks(1));
    REQUIRE(empty);
    CHECK(gazeFacesOfStored(*empty).empty());
    CHECK(empty->planes.size() == 1);
    // What is not a gaze frame has no faces; a table the model got wrong is an error.
    CHECK(gazeFacesOfStored(StoredFrame{}).empty());
    CHECK(gazeFacesOfStored(packRgba8(pattern(8, 8))).empty());
    CHECK_FALSE(gazeStoredFrame(Picture{}, Ticks{}, Ticks{}).has_value());
}

TEST_CASE("A gaze chain runs the landmark model at the working size", "[ai][video][gaze]") {
    FakeBackend backend;
    const auto made = processPicture(pattern(2560, 1440), aiGazeChain(), backend, nullptr, {});
    REQUIRE(made);
    REQUIRE(backend.calls.size() == 1);
    CHECK(backend.calls[0].model == Model::FaceLandmarks);
    CHECK(backend.calls[0].width == 1280);
    CHECK(backend.calls[0].height == 720);
    // The answer is the table, whatever its size is.
    CHECK(made->width == kGazeSlotCount);
    const auto parsed = gazeTableFromPicture(*made);
    REQUIRE(parsed);
    CHECK(parsed->workWidth == 1280);
    CHECK(parsed->workHeight == 720);
    CHECK(parsed->faces.size() == 1);
    // A smaller picture is looked at as it is; alpha is not part of the measurements.
    FakeBackend second;
    const auto small = processPicture(pattern(320, 180, 0, true), aiGazeChain(), second, nullptr, {});
    REQUIRE(small);
    CHECK(second.calls[0].width == 320);
    CHECK_FALSE(small->hasAlpha());
}

TEST_CASE("A gaze job stores the measurements of every frame and skips what the cache has", "[ai][video][gaze]") {
    ProcessedCache cache(freshDir("gaze-job"));
    FakeBackend backend;
    backend.gaze.down = 0.15;
    FakeSource source(3, 320, 180);
    Runner runner(source, backend, cache);
    const Job job = gazeJob(3);
    const auto first = runner.run(job, {}, {});
    REQUIRE(first);
    CHECK(first->processed == 3);
    CHECK(backend.calls.size() == 3);
    CHECK(cache.frames(job.identity).size() == 3);
    const auto stored = cache.get(job.identity, frameTicks(1), Ticks{frameTicks(1).value / 2});
    REQUIRE(stored);
    const std::vector<GazeFace> faces = gazeFacesOfStored(*stored);
    REQUIRE(faces.size() == 1);
    CHECK(faces[0].eyes[0].irisY == Approx(0.5 * (0.007 - 0.012) + 0.15 * 2.0 * 0.045).margin(5e-5));
    CHECK(faces[0].eyes[0].valid());
    // Again: nothing is made.
    const auto again = runner.run(job, {}, {});
    REQUIRE(again);
    CHECK(again->processed == 0);
    CHECK(again->skipped == 3);
    CHECK(backend.calls.size() == 3);
    // A frame without a face is stored too (it is measured: there is nothing to correct), so it counts as made.
    ProcessedCache bare(freshDir("gaze-job-empty"));
    FakeBackend nobody;
    nobody.gazeFaces = 0;
    FakeSource source2(2, 64, 36);
    Runner runner2(source2, nobody, bare);
    REQUIRE(runner2.run(gazeJob(2), {}, {}));
    CHECK(bare.frames(gazeJob(2).identity).size() == 2);
    const auto none = bare.get(gazeJob(2).identity, Ticks{}, frameTicks(1));
    REQUIRE(none);
    CHECK(gazeFacesOfStored(*none).empty());
    // A table the model got wrong is an error and nothing half-made is stored.
    class WrongTable final : public IVideoBackend {
    public:
        std::expected<StepResult, Error> run(const Picture&, const Step&, std::stop_token) override {
            StepResult r;
            r.picture = pattern(10, 10);
            return r;
        }
    } wrong;
    ProcessedCache untouched(freshDir("gaze-job-wrong"));
    FakeSource source3(2, 64, 36);
    Runner runner3(source3, wrong, untouched);
    const auto refused = runner3.run(gazeJob(2), {}, {});
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == ErrorCode::DecoderError);
    CHECK(untouched.frames(gazeJob(2).identity).empty());
}

TEST_CASE("A clip with AI Eye Contact has a gaze target shared by every Eye Contact; a disabled one has none", "[ai][video][gaze][targets]") {
    ProjectState s;
    auto media = mediaOf(s, 960, 540);
    s.media = {media};
    const auto effect = [&](std::string_view id, bool enabled) {
        EffectInstance e;
        e.id = s.ids.allocate<EffectInstanceId>();
        e.effect = std::string(id);
        e.enabled = enabled;
        return e;
    };
    Clip clip;
    clip.id = s.ids.allocate<ClipId>();
    clip.media = media->id;
    clip.timelineRange = {Ticks{}, Ticks::fromSeconds(2)};
    clip.setEffects({effect(kAiEyeContactEffect, true), effect(kAiEyeContactEffect, true)});
    const Sequence seq = sequenceOf(s, {clip});
    const Clip& placed = seq.videoTracks.front()->clips.front();
    const auto t = gazeTarget(s, seq, placed, 0);
    REQUIRE(t.has_value());
    CHECK(t->kind == JobKind::Gaze);
    CHECK(t->chain.stages.size() == 1); // two Eye Contact effects, one set of measurements
    CHECK(t->expected == 50);
    CHECK(t->identity == identityOf(*media, 0, aiGazeChain()));
    CHECK(t->identity != identityOf(*media, 0, aiFaceChain())); // their pictures never share a folder
    CHECK(aiChainLabel(t->chain) == "Eye measurements");
    // It is not a source-stage effect, and makes no face or depth maps.
    const auto pictures = clipTarget(s, seq, placed, 0);
    REQUIRE(pictures.has_value());
    CHECK(pictures->chain.empty());
    CHECK_FALSE(faceTarget(s, seq, placed, 0).has_value());
    CHECK_FALSE(depthTarget(s, seq, placed, 0).has_value());
    // A disabled Eye Contact makes no measurements.
    Clip off = placed;
    off.setEffects({effect(kAiEyeContactEffect, false)});
    CHECK_FALSE(gazeTarget(s, seq, off, 0).has_value());
    // The sequence lists the gaze target; with AI Face Retouch beside it the clip has both, each its own job.
    const auto all = sequenceTargets(s, seq, std::nullopt);
    REQUIRE(all.size() == 1);
    CHECK(all[0].kind == JobKind::Gaze);
    Clip both = placed;
    both.setEffects({effect(kAiEyeContactEffect, true), effect(kAiFaceRetouchEffect, true)});
    const Sequence seq2 = sequenceOf(s, {both});
    const auto twice = sequenceTargets(s, seq2, std::nullopt);
    REQUIRE(twice.size() == 2);
    CHECK(std::set<JobKind>{twice[0].kind, twice[1].kind} == std::set<JobKind>{JobKind::Faces, JobKind::Gaze});
    // How much of it is made: counted from the cache, frame by frame.
    ProcessedCache cache(freshDir("gaze-targets"));
    CHECK(coveredCount(cache, *t) == 0);
    FakeBackend backend;
    FakeSource source(3, 64, 36);
    Runner runner(source, backend, cache);
    Job job = gazeJob(3);
    job.identity = t->identity;
    REQUIRE(runner.run(job, {}, {}));
    CHECK(coveredCount(cache, *t) == 3);
}

TEST_CASE("HelperBackend makes the eye measurements through the scripted helper and loads the detector with the landmark model", "[ai][video][gaze][helper]") {
    std::set<Model> resolved;
    HelperBackendOptions o;
    o.executable = WABCUT_FAKE_EMBED;
    o.arguments = {"--mode", "normal"};
    o.workDir = freshDir("gaze-helper");
    o.resolveModel = [&](Model model) {
        resolved.insert(model);
        return std::optional<std::filesystem::path>{"fake-model.onnx"};
    };
    o.startTimeout = std::chrono::seconds(20);
    o.loadTimeout = std::chrono::seconds(20);
    o.requestBase = std::chrono::seconds(20);
    HelperBackend backend(o);
    Step step;
    step.model = Model::FaceLandmarks;
    const auto made = backend.run(pattern(640, 360), step, {});
    REQUIRE(made);
    CHECK(resolved == std::set<Model>{Model::FaceLandmarks, Model::FaceDetect});
    // The answer is a table of one face, not a picture of the input's size.
    CHECK(made->picture.width == kGazeSlotCount);
    CHECK(made->picture.height == 1);
    const auto parsed = gazeTableFromPicture(made->picture);
    REQUIRE(parsed);
    CHECK(parsed->workWidth == 640);
    CHECK(parsed->workHeight == 360);
    REQUIRE(parsed->faces.size() == 1);
    const GazeFace face = gazeFaceOf(parsed->faces.front(), parsed->workWidth, parsed->workHeight);
    CHECK(face.eyes[0].valid());
    CHECK(face.eyes[0].halfWidth == Approx(0.045).margin(1e-4));
    CHECK(face.eyes[0].irisY == Approx(0.5 * (0.007 - 0.012) + 0.12 * 2.0 * 0.045).margin(1e-4));
    CHECK(made->device == "FakeCPU");
}

#ifdef WABCUT_EMBED_HELPER
namespace {

/// A binary PPM (P6, 8-bit) as a 16-bit picture.
std::optional<Picture> readPpm(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::string magic;
    int width = 0;
    int height = 0;
    int maxValue = 0;
    in >> magic >> width >> height >> maxValue;
    in.get();
    if (magic != "P6" || width <= 0 || height <= 0 || maxValue != 255) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3);
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!in) {
        return std::nullopt;
    }
    Picture p;
    p.width = width;
    p.height = height;
    p.rgb.resize(bytes.size());
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        p.rgb[i] = static_cast<std::uint16_t>(bytes[i] * 257u);
    }
    return p;
}

} // namespace

TEST_CASE("The real landmark model: YuNet finds the face and MediaPipe's network measures its eyes (WABCUT_TEST_VIDEO_MODELS_DIR, WABCUT_TEST_FACE_IMAGE)",
          "[ai][video][gaze][live]") {
    const char* dir = std::getenv("WABCUT_TEST_VIDEO_MODELS_DIR");
    const char* image = std::getenv("WABCUT_TEST_FACE_IMAGE");
    if (dir == nullptr || *dir == '\0' || image == nullptr || *image == '\0') {
        WARN("skipped: set WABCUT_TEST_VIDEO_MODELS_DIR (face_landmarks_mediapipe.onnx, face_detection_yunet_2023mar.onnx) and WABCUT_TEST_FACE_IMAGE (a P6 PPM of a face)");
        return;
    }
    const std::filesystem::path landmarkFile = std::filesystem::path(dir) / "face_landmarks_mediapipe.onnx";
    const std::filesystem::path detectorFile = std::filesystem::path(dir) / "face_detection_yunet_2023mar.onnx";
    REQUIRE(std::filesystem::is_regular_file(landmarkFile));
    REQUIRE(std::filesystem::is_regular_file(detectorFile));
    const auto photo = readPpm(image);
    REQUIRE(photo.has_value());
    Step step;
    step.model = Model::FaceLandmarks;
    std::vector<GazeFace> found;
    for (const char* device : {"auto", "cpu"}) {
        HelperBackendOptions o;
        o.executable = WABCUT_EMBED_HELPER;
        o.device = device;
        o.workDir = freshDir(std::string("gaze-live-") + device);
        o.resolveModel = [&](Model model) { return std::optional<std::filesystem::path>{model == Model::FaceLandmarks ? landmarkFile : detectorFile}; };
        o.startTimeout = std::chrono::seconds(60);
        o.loadTimeout = std::chrono::seconds(120);
        o.requestBase = std::chrono::seconds(120);
        HelperBackend backend(o);
        const auto started = std::chrono::steady_clock::now();
        const auto first = backend.run(*photo, step, {}); // the first run loads the graphs
        REQUIRE(first);
        const auto middle = std::chrono::steady_clock::now();
        const auto second = backend.run(*photo, step, {});
        REQUIRE(second);
        const auto finished = std::chrono::steady_clock::now();
        const double firstMs = std::chrono::duration<double, std::milli>(middle - started).count();
        const double secondMs = std::chrono::duration<double, std::milli>(finished - middle).count();
        WARN(device << " (" << second->device << "): first picture " << firstMs << " ms, second " << secondMs << " ms");
        const auto table = gazeTableFromPicture(second->picture);
        REQUIRE(table);
        REQUIRE(table->faces.size() == 1);
        REQUIRE(table->workWidth == photo->width);
        const GazeFace face = gazeFaceOf(table->faces.front(), table->workWidth, table->workHeight);
        for (std::size_t e = 0; e < 2; ++e) {
            const GazeEye& eye = face.eyes[e];
            INFO(device << " eye " << e);
            CHECK(eye.valid());
            CHECK(eye.halfWidth > 0.006);  // an eye is 1 - 7 % of the picture's width in a portrait
            CHECK(eye.halfWidth < 0.06);
            CHECK(eye.upper + eye.lower > 0.0); // an opening
            CHECK(eye.irisRadius > 0.4 * eye.halfWidth * 0.3);
            CHECK(std::abs(eye.irisX) < 0.8 * eye.halfWidth); // the iris is inside the eye
            CHECK(eye.cy > 0.15);
            CHECK(eye.cy < 0.6);
        }
        CHECK(face.eyes[0].cx < face.eyes[1].cx); // the picture's left eye first
        CHECK(face.score > 0.9);
        CHECK(face.turn < 0.4);                   // a portrait facing the camera
        WARN(device << ": eye 0 at (" << face.eyes[0].cx << ", " << face.eyes[0].cy << ") half width " << face.eyes[0].halfWidth << " iris (" << face.eyes[0].irisX
                    << ", " << face.eyes[0].irisY << ") radius " << face.eyes[0].irisRadius << "; turn " << face.turn);
        // The measurements for the render suite's opt-in test on the same photo, and the landmarks over the picture for looking at.
        {
            std::filesystem::create_directories(std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "ai-video");
            if (std::string(device) == "auto") {
                std::ofstream out(std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "ai-video" / "gaze-face.txt");
                out.precision(12);
                for (const GazeEye& eye : face.eyes) {
                    out << eye.cx << ' ' << eye.cy << ' ' << eye.angle << ' ' << eye.halfWidth << ' ' << eye.upper << ' ' << eye.lower << ' ' << eye.irisX
                        << ' ' << eye.irisY << ' ' << eye.irisRadius << '\n';
                }
                out << face.turn << ' ' << face.score << ' ' << face.size << '\n';
            }
            std::ofstream out(std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "ai-video" / (std::string("gaze-overlay-") + device + ".ppm"), std::ios::binary);
            out << "P6\n" << photo->width << " " << photo->height << "\n255\n";
            std::vector<std::uint8_t> bytes(photo->rgb.size());
            for (std::size_t i = 0; i < bytes.size(); ++i) {
                bytes[i] = static_cast<std::uint8_t>(photo->rgb[i] / 257);
            }
            for (std::size_t i = 0; i < table->faces.front().points.size(); ++i) {
                const LandmarkPoint& p = table->faces.front().points[i];
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int x = static_cast<int>(std::lround(p.x)) + dx;
                        const int y = static_cast<int>(std::lround(p.y)) + dy;
                        if (x >= 0 && y >= 0 && x < photo->width && y < photo->height) {
                            std::uint8_t* px = &bytes[(static_cast<std::size_t>(y) * static_cast<std::size_t>(photo->width) + static_cast<std::size_t>(x)) * 3];
                            const bool iris = i >= 32 && i < 42;
                            px[0] = iris ? 255 : 0;
                            px[1] = iris ? 0 : 255;
                            px[2] = 0;
                        }
                    }
                }
            }
            out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
        found.push_back(face);
    }
    // The GPU and the CPU agree to a pixel or two.
    REQUIRE(found.size() == 2);
    for (std::size_t e = 0; e < 2; ++e) {
        CHECK(std::abs(found[0].eyes[e].irisX - found[1].eyes[e].irisX) < 0.003);
        CHECK(std::abs(found[0].eyes[e].irisY - found[1].eyes[e].irisY) < 0.003);
        CHECK(std::abs(found[0].eyes[e].cx - found[1].eyes[e].cx) < 0.002);
    }
}
#endif
