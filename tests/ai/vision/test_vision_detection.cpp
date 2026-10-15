// Subject detection post-processing (AI-9) on fixture tensors: letterboxing, the networks' input tensors, YuNet's and
// YOLOX's decoding, non-maximum suppression, the ByteTrack-style tracker (ids through occlusion, low-score extension,
// reset at cuts) and the saliency fallback. No model runs here.

#include "VisionTestUtil.h"

#include <wabcut/ai/search/EmbedProtocol.h>
#include <wabcut/ai/vision/Detection.h>
#include <wabcut/ai/vision/Saliency.h>
#include <wabcut/ai/vision/Tracker.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace wabcut;
using namespace wabcut::ai::vision;
using namespace wabcut::ai::vision::test;
using Catch::Approx;

namespace {

bool near(const Box& a, const Box& b, float tolerance = 0.51f) {
    return std::abs(a.x - b.x) <= tolerance && std::abs(a.y - b.y) <= tolerance && std::abs(a.w - b.w) <= tolerance &&
           std::abs(a.h - b.h) <= tolerance;
}

/// YuNet outputs for a 640 x 640 input, all zero (no face) until set.
struct YuNetTensors {
    YuNetTensors() {
        for (std::size_t s = 0; s < 3; ++s) {
            const std::size_t cells = static_cast<std::size_t>(640 / kStrides[s]) * static_cast<std::size_t>(640 / kStrides[s]);
            cls[s].assign(cells, 0.0f);
            obj[s].assign(cells, 0.0f);
            bbox[s].assign(cells * 4, 0.0f);
            kps[s].assign(cells * 10, 0.0f);
        }
    }
    void set(std::size_t s, int row, int col, float c, float o, std::array<float, 4> b) {
        const std::size_t idx = static_cast<std::size_t>(row) * static_cast<std::size_t>(640 / kStrides[s]) + static_cast<std::size_t>(col);
        cls[s][idx] = c;
        obj[s][idx] = o;
        for (std::size_t i = 0; i < 4; ++i) {
            bbox[s][idx * 4 + i] = b[i];
        }
    }
    YuNetOutputs view() const {
        YuNetOutputs out;
        for (std::size_t s = 0; s < 3; ++s) {
            out.cls[s] = cls[s];
            out.obj[s] = obj[s];
            out.bbox[s] = bbox[s];
            out.kps[s] = kps[s];
        }
        return out;
    }
    static constexpr int kStrides[3] = {8, 16, 32};
    std::array<std::vector<float>, 3> cls, obj, bbox, kps;
};

/// YOLOX's 3549 x 85 prediction for a 416 x 416 input, all zero until set.
struct YoloxTensor {
    YoloxTensor() : data(3549 * 85, 0.0f) {}
    void set(int strideIndex, int gx, int gy, std::array<float, 4> b, float objectness, int cls, float classScore) {
        static constexpr int kStrides[3] = {8, 16, 32};
        std::size_t row = 0;
        for (int s = 0; s < strideIndex; ++s) {
            row += static_cast<std::size_t>(416 / kStrides[s]) * static_cast<std::size_t>(416 / kStrides[s]);
        }
        row += static_cast<std::size_t>(gy) * static_cast<std::size_t>(416 / kStrides[strideIndex]) + static_cast<std::size_t>(gx);
        float* p = data.data() + row * 85;
        for (std::size_t i = 0; i < 4; ++i) {
            p[i] = b[i];
        }
        p[4] = objectness;
        p[5 + cls] = classScore;
    }
    std::vector<float> data;
};

} // namespace

TEST_CASE("Letterboxes map network boxes back to the picture", "[ai][vision]") {
    const Letterbox wide = letterboxFor(1280, 720, 640, 640);
    CHECK(wide.scale == Approx(0.5f));
    CHECK(near(wide.toImage(Box{10, 20, 100, 50}), Box{20, 40, 200, 100}));
    CHECK(near(wide.toImage(Box{600, 300, 100, 100}), Box{1200, 600, 80, 120})); // clipped to the picture
    const Letterbox tall = letterboxFor(720, 1280, 416, 416);
    CHECK(tall.scale == Approx(416.0f / 1280.0f));
    CHECK(letterboxFor(0, 0, 640, 640).toImage(Box{1, 1, 1, 1}).area() == 0.0f);

    CHECK(iou(Box{0, 0, 10, 10}, Box{0, 0, 10, 10}) == Approx(1.0f));
    CHECK(iou(Box{0, 0, 10, 10}, Box{20, 0, 10, 10}) == 0.0f);
    CHECK(iou(Box{0, 0, 10, 10}, Box{5, 0, 10, 10}) == Approx(50.0f / 150.0f));
    CHECK(iou(Box{0, 0, 0, 0}, Box{0, 0, 0, 0}) == 0.0f);
    CHECK(near(normalized(Box{64, 36, 128, 72}, 1280, 720), Box{0.05f, 0.05f, 0.1f, 0.1f}, 1e-6f));
}

TEST_CASE("The networks' input tensors follow their reference preprocessing", "[ai][vision]") {
    const Image picture = canvas(1280, 640, {Block{Box{0, 0, 1, 1}, 10, 20, 30}});
    Letterbox box;
    const std::vector<float> yunet = yunetInput(picture, box);
    REQUIRE(yunet.size() == 3u * 640u * 640u);
    CHECK(box.scale == Approx(0.5f));
    const std::size_t plane = 640 * 640;
    CHECK(yunet[0] == 30.0f);          // B first, 0..255
    CHECK(yunet[plane] == 20.0f);      // G
    CHECK(yunet[2 * plane] == 10.0f);  // R
    CHECK(yunet[400 * 640 + 10] == 0.0f); // below the picture: black padding
    CHECK(yunet[2 * plane + 319 * 640 + 639] == 10.0f);

    const std::vector<float> yolox = yoloxInput(picture, box);
    REQUIRE(yolox.size() == 3u * 416u * 416u);
    const std::size_t p416 = 416 * 416;
    CHECK(yolox[0] == 30.0f);           // B first, 0..255, not normalised (the released weights)
    CHECK(yolox[p416] == 20.0f);        // G
    CHECK(yolox[2 * p416] == 10.0f);    // R
    CHECK(yolox[400 * 416 + 5] == 114.0f); // grey 114 padding

    // An empty picture gives the padding only (no crash).
    CHECK(yunetInput(Image{}, box).size() == 3u * 640u * 640u);
}

TEST_CASE("YuNet outputs decode to faces with landmarks after the threshold and NMS", "[ai][vision]") {
    YuNetTensors t;
    // Stride 8, cell (row 10, col 20): a 32 x 32 face centred at (164, 84) in the input.
    t.set(0, 10, 20, 0.9f, 0.9f, {0.5f, 0.5f, std::log(4.0f), std::log(4.0f)});
    // Its neighbour: the same face, less sure (suppressed).
    t.set(0, 10, 21, 0.8f, 0.8f, {-0.4f, 0.5f, std::log(4.0f), std::log(4.0f)});
    // Stride 32: a big face elsewhere; and a weak cell under the threshold.
    t.set(2, 5, 12, 0.95f, 0.85f, {0.0f, 0.0f, std::log(4.0f), std::log(5.0f)});
    t.set(1, 3, 3, 0.5f, 0.5f, {0.0f, 0.0f, 0.0f, 0.0f});
    const Letterbox box = letterboxFor(1280, 720, 640, 640);
    const auto faces = decodeYuNet(t.view(), box, 0.6f, 0.3f);
    REQUIRE(faces.size() == 2);
    CHECK(faces[0].score == Approx(std::sqrt(0.9f * 0.9f))); // best first
    CHECK(near(faces[0].box, Box{296, 136, 64, 64}));        // input pixels / 0.5
    REQUIRE(faces[0].landmarks.size() == 10);
    CHECK(faces[0].landmarks[0] == Approx(20.0f * 8.0f / 0.5f)); // (kps + col) * stride / scale
    CHECK(faces[0].landmarks[1] == Approx(10.0f * 8.0f / 0.5f));
    CHECK(faces[1].score == Approx(std::sqrt(0.95f * 0.85f)));
    CHECK(near(faces[1].box, Box{(12 * 32 - 64) * 2.0f, (5 * 32 - 80) * 2.0f, 256, 320}));

    // A malformed output (too short) yields nothing from that stride, never a crash.
    YuNetOutputs broken = t.view();
    broken.bbox[0] = broken.bbox[0].first(10);
    CHECK(decodeYuNet(broken, box).size() == 1);
}

TEST_CASE("YOLOX predictions decode to people; other classes are dropped unless asked for", "[ai][vision]") {
    YoloxTensor t;
    t.set(0, 10, 5, {0.5f, 0.5f, std::log(2.0f), std::log(4.0f)}, 0.9f, kCocoPerson, 0.8f); // a person
    t.set(0, 11, 5, {0.0f, 0.5f, std::log(2.0f), std::log(4.0f)}, 0.8f, kCocoPerson, 0.7f); // the same person
    t.set(1, 3, 5, {0.0f, 0.0f, std::log(3.0f), std::log(3.0f)}, 0.95f, 2, 0.9f);           // a car
    t.set(2, 1, 1, {0.0f, 0.0f, 0.0f, 0.0f}, 0.3f, kCocoPerson, 0.5f);                     // too weak
    const Letterbox box = letterboxFor(832, 416, 416, 416);
    const auto people = decodeYolox(t.data, box);
    REQUIRE(people.size() == 1);
    CHECK(people[0].classId == kCocoPerson);
    CHECK(people[0].score == Approx(0.72f));
    CHECK(near(people[0].box, Box{152, 56, 32, 64})); // input (76, 28, 16, 32) / 0.5

    const auto all = decodeYolox(t.data, box, -1);
    REQUIRE(all.size() == 2);
    CHECK(all[0].classId == 2); // the car scores 0.855
    CHECK(all[1].classId == kCocoPerson);

    CHECK(decodeYolox(std::span<const float>(t.data).first(1000), box).empty()); // truncated: what fits, here none
}

TEST_CASE("Non-maximum suppression keeps the best box per class", "[ai][vision]") {
    std::vector<Detection> found{
        Detection{Box{0, 0, 10, 10}, 0.6f, 0, {}},
        Detection{Box{1, 1, 10, 10}, 0.9f, 0, {}},
        Detection{Box{1, 1, 10, 10}, 0.5f, 1, {}}, // another class: kept
        Detection{Box{50, 50, 10, 10}, 0.4f, 0, {}},
    };
    const auto kept = nonMaximumSuppression(found, 0.45f);
    REQUIRE(kept.size() == 3);
    CHECK(kept[0].score == 0.9f);
    CHECK(kept[1].classId == 1);
    CHECK(kept[2].score == 0.4f);
}

TEST_CASE("The tracker keeps ids through occlusion and resets at cuts", "[ai][vision][tracker]") {
    ByteTracker tracker;
    const auto at = [](float x, float score = 0.9f) { return Detection{Box{x, 100, 60, 60}, score, 0, {}}; };
    std::vector<int> ids;
    for (int i = 0; i < 6; ++i) { // moving right, 8 px per sample
        const Detection d = at(100.0f + 8.0f * static_cast<float>(i));
        ids.push_back(tracker.update(std::span(&d, 1))[0]);
    }
    CHECK(std::ranges::all_of(ids, [&](int id) { return id == ids[0]; }));
    REQUIRE(tracker.tracks().size() == 1);
    CHECK(tracker.tracks()[0].confirmed);
    CHECK(tracker.tracks()[0].velocity.x == Approx(8.0f).margin(2.0f));

    // Occluded for 5 samples, then back where the motion predicts: the same id.
    for (int i = 0; i < 5; ++i) {
        CHECK(tracker.update({}).empty());
    }
    const Detection back = at(100.0f + 8.0f * 11.0f);
    CHECK(tracker.update(std::span(&back, 1))[0] == ids[0]);

    // A dim detection (low score) extends the track but cannot start one.
    const Detection dim = at(100.0f + 8.0f * 12.0f, 0.3f);
    CHECK(tracker.update(std::span(&dim, 1))[0] == ids[0]);
    const Detection lonelyDim{Box{900, 500, 40, 40}, 0.3f, 0, {}};
    CHECK(tracker.update(std::span(&lonelyDim, 1))[0] == -1);

    // Two subjects far apart keep distinct ids.
    const std::vector<Detection> two{at(300), Detection{Box{800, 100, 60, 60}, 0.9f, 0, {}}};
    const auto both = tracker.update(two);
    CHECK(both[0] != both[1]);

    // A cut: everything is forgotten, new ids keep counting up.
    tracker.reset();
    CHECK(tracker.tracks().empty());
    const Detection after = at(100);
    CHECK(tracker.update(std::span(&after, 1))[0] > std::max(both[0], both[1]));

    // Lost for longer than maxLost: a returning subject gets a new id.
    TrackerOptions shortMemory;
    shortMemory.maxLost = 2;
    ByteTracker forgetful(shortMemory);
    const Detection d = at(100);
    const int first = forgetful.update(std::span(&d, 1))[0];
    for (int i = 0; i < 4; ++i) {
        forgetful.update({});
    }
    CHECK(forgetful.update(std::span(&d, 1))[0] != first);
}

TEST_CASE("Saliency finds the thing that stands out, and motion", "[ai][vision][saliency]") {
    const Image picture = canvas(320, 180, {Block{Box{0.6f, 0.3f, 0.12f, 0.2f}, 250, 250, 250}}, 40);
    const GrayImage gray = toGray(picture);
    CHECK(gray.width == 64);
    CHECK(gray.height == 36);
    const auto region = salientRegion(gray, nullptr);
    REQUIRE(region);
    CHECK(region->box.cx() == Approx(0.66f).margin(0.08f));
    CHECK(region->box.cy() == Approx(0.4f).margin(0.1f));
    CHECK(region->strength > 1.0f);

    CHECK_FALSE(salientRegion(toGray(canvas(320, 180, {}, 128)), nullptr)); // flat: nothing stands out
    CHECK(motionEnergy(gray, gray) == 0.0f);
    const GrayImage moved = toGray(canvas(320, 180, {Block{Box{0.1f, 0.3f, 0.12f, 0.2f}, 250, 250, 250}}, 40));
    CHECK(motionEnergy(gray, moved) > 0.01f);
    CHECK(motionEnergy(gray, GrayImage{}) == 0.0f);
    CHECK(spectralResidual(GrayImage{}).empty());
}

TEST_CASE("The detect messages of the wabcut-embed protocol round-trip and refuse malformed rows", "[ai][vision][protocol]") {
    namespace embed = ai::search::embed;
    embed::Load load;
    load.id = "1";
    load.faces = "C:/models/yunet.onnx";
    load.objects = "C:/models/yolox.onnx";
    auto loaded = embed::decodeRequest(embed::encode(embed::Request{load}));
    REQUIRE(loaded);
    CHECK(std::get<embed::Load>(*loaded).faces == load.faces);
    CHECK(std::get<embed::Load>(*loaded).objects == load.objects);
    CHECK_FALSE(embed::decodeRequest(R"({"type":"load","id":"1"})")); // names no model

    embed::Detect detect;
    detect.id = "2";
    detect.images = embed::ImageBatch{"C:/temp/detect.rgb", 640, 360, 4};
    detect.objects = false;
    detect.faceThreshold = 0.7f;
    auto decoded = embed::decodeRequest(embed::encode(embed::Request{detect}));
    REQUIRE(decoded);
    const auto& back = std::get<embed::Detect>(*decoded);
    CHECK(back.images.count == 4);
    CHECK(back.faces);
    CHECK_FALSE(back.objects);
    CHECK(back.faceThreshold == Approx(0.7f));
    CHECK_FALSE(embed::decodeRequest(R"({"type":"detect","id":"2","images":{"path":"x","width":5000,"height":10,"count":1}})"));
    CHECK_FALSE(embed::decodeRequest(R"({"type":"detect","id":"2"})"));

    embed::Detections reply{"2", {}, 3.5};
    embed::DetectedFrame frame;
    frame.faces.push_back(std::vector<float>(embed::kFaceRow, 1.0f));
    frame.objects.push_back({1, 2, 3, 4, 0.5f, 0});
    reply.frames = {frame, embed::DetectedFrame{}};
    auto answer = embed::decodeResponse(embed::encode(embed::Response{reply}));
    REQUIRE(answer);
    const auto& got = std::get<embed::Detections>(*answer);
    REQUIRE(got.frames.size() == 2);
    CHECK(got.frames[0].faces[0].size() == embed::kFaceRow);
    CHECK(got.frames[0].objects[0][3] == 4.0f);
    CHECK(got.frames[1].faces.empty());
    CHECK(got.milliseconds == Approx(3.5));
    CHECK(embed::responseId(*answer) == "2");
    CHECK_FALSE(embed::decodeResponse(R"({"type":"detections","id":"2","frames":[{"faces":[[1,2,3]]}]})"));
    CHECK_FALSE(embed::decodeResponse(R"({"type":"detections","id":"2","frames":[{"objects":[[1,2,3,4,"x",0]]}]})"));
    CHECK_FALSE(embed::decodeResponse(R"({"type":"detections","id":"2"})"));
}
