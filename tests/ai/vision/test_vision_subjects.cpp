// The subject-analysis pipeline (AI-9) on scripted videos: shot-aware adaptive sampling, batched detection, tracks reset
// at cuts, speaking activity, the saliency fallback, cancelling, the cache JSON; and HelperDetector against the scripted
// wabcut-embed (detections, batching, big pictures, crash, hang, garbage, restart).

#include "VisionTestUtil.h"

#include <wabcut/ai/vision/HelperDetector.h>
#include <wabcut/ai/vision/Subjects.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <random>
#include <set>
#include <thread>

using namespace wabcut;
using namespace wabcut::ai::vision;
using namespace wabcut::ai::vision::test;
using Catch::Approx;

namespace {

/// A face drifting right over 4 s; a cut at 2 s moves it to the left half.
Image driftingFace(Ticks t) {
    const double s = t.toSeconds();
    const float x = s < 2.0 ? 0.55f + 0.02f * static_cast<float>(s) : 0.15f + 0.02f * static_cast<float>(s - 2.0);
    return canvas(160, 90, {person(x - 0.03f, 0.15f, 0.2f, 0.8f), face(x, 0.2f, 0.12f, 0.3f)}); // the face in front
}

std::size_t samplesIn(const SubjectAnalysis& a, double from, double to) {
    return static_cast<std::size_t>(std::ranges::count_if(a.samples, [&](const SubjectSample& s) {
        return s.time >= seconds(from) && s.time < seconds(to);
    }));
}

} // namespace

TEST_CASE("Subjects are sampled per shot, tracked, and reset at cuts", "[ai][vision][subjects]") {
    FunctionVideo video(seconds(4), driftingFace);
    BlockDetector detector;
    SubjectOptions options;
    options.batch = 4;
    std::vector<double> progress;
    auto result = analyzeSubjects(video, &detector, shotsOf(seconds(4), {2.0}), options, [&](double p) {
        progress.push_back(p);
        return true;
    });
    REQUIRE(result);
    const SubjectAnalysis& a = *result;
    CHECK(a.models == "blocks/1");
    CHECK(a.frameWidth == 160);
    CHECK(a.frameHeight == 90);
    REQUIRE(a.cuts == std::vector<Ticks>{seconds(2)});
    CHECK(a.shotCount() == 2);
    CHECK(a.shotOf(seconds(1)) == 0);
    CHECK(a.shotOf(seconds(2)) == 1);
    CHECK(a.shot(1).start == seconds(2));
    CHECK(a.shot(1).end() == seconds(4));

    // A slow drift is below the motion threshold: 5 samples per second, the first half a step into each shot.
    CHECK(samplesIn(a, 0, 2) == 10);
    CHECK(samplesIn(a, 2, 4) == 10);
    CHECK(a.samples.front().time == seconds(0.1));
    CHECK(std::ranges::all_of(detector.batchSizes, [](std::size_t n) { return n <= 4; }));
    CHECK(std::ranges::is_sorted(progress));
    CHECK(progress.back() == Approx(1.0).margin(0.06));

    // Every sample found the face and the person, in normalised coordinates.
    std::set<int> before;
    std::set<int> after;
    for (const SubjectSample& s : a.samples) {
        REQUIRE(s.faces.size() == 1);
        REQUIRE(s.people.size() == 1);
        CHECK(s.faces[0].box.w == Approx(0.125f).margin(0.01f));
        (s.time < seconds(2) ? before : after).insert(s.faces[0].track);
        CHECK(s.salient.has_value());
    }
    CHECK(before.size() == 1); // one track per shot ...
    CHECK(after.size() == 1);
    CHECK(*before.begin() != *after.begin()); // ... reset at the cut
    CHECK(a.samples[3].faces[0].box.x > a.samples[1].faces[0].box.x); // drifting right

    // The cache form round-trips (numbers rounded to 1e-4).
    const std::string json = subjectsToJson(a);
    const auto back = subjectsFromJson(json);
    REQUIRE(back);
    CHECK(subjectsToJson(*back) == json);
    CHECK(back->samples.size() == a.samples.size());
    CHECK(back->samples[0].faces[0].track == a.samples[0].faces[0].track);
    CHECK_FALSE(subjectsFromJson("{\"version\":99}"));
    CHECK_FALSE(subjectsFromJson("not json"));
    CHECK_FALSE(subjectsFromJson(R"({"version":1,"start":0,"end":10,"samples":[{"t":1,"f":[[1,2]]}]})"));
}

TEST_CASE("Sampling gets denser while the picture moves", "[ai][vision][subjects]") {
    // A bright block sweeping across a dark frame: lots of motion.
    FunctionVideo busy(seconds(2), [](Ticks t) {
        const double sweep = t.toSeconds() * 2.7;
        const float x = 0.05f + 0.7f * static_cast<float>(sweep - std::floor(sweep));
        return canvas(160, 90, {Block{Box{x, 0.2f, 0.2f, 0.4f}, 250, 250, 250}}, 30);
    });
    BlockDetector detector;
    auto moving = analyzeSubjects(busy, &detector, shotsOf(seconds(2)));
    REQUIRE(moving);
    CHECK(moving->samples.size() >= 17); // ~10 per second once moving
    CHECK(std::ranges::any_of(moving->samples, [](const SubjectSample& s) { return s.motion > 0.035f; }));

    FunctionVideo still(seconds(2), [](Ticks) { return canvas(160, 90, {face(0.4f, 0.2f, 0.2f, 0.4f)}); });
    auto calm = analyzeSubjects(still, &detector, shotsOf(seconds(2)));
    REQUIRE(calm);
    CHECK(calm->samples.size() == 10);
    CHECK(std::ranges::all_of(calm->samples, [](const SubjectSample& s) { return s.motion == 0.0f; }));

    SubjectOptions capped;
    capped.maxSamples = 3;
    auto few = analyzeSubjects(still, &detector, shotsOf(seconds(2)), capped);
    REQUIRE(few);
    CHECK(few->samples.size() == 3);
}

TEST_CASE("A talking face is more active than a silent one", "[ai][vision][subjects]") {
    // Two faces; the left one's mouth (lower half) flickers between samples.
    FunctionVideo talk(seconds(2), [](Ticks t) {
        const bool open = static_cast<int>(t.toSeconds() * 5.0) % 2 == 1;
        std::vector<Block> blocks{face(0.1f, 0.2f, 0.15f, 0.4f), face(0.7f, 0.2f, 0.15f, 0.4f)};
        if (open) {
            blocks.push_back(Block{Box{0.13f, 0.45f, 0.09f, 0.1f}, 40, 0, 0}); // a dark mouth inside the left face
        }
        return canvas(320, 180, blocks);
    });
    // The block detector finds one red box per picture; use a detector that reports both faces.
    class TwoFaces : public SubjectDetector {
    public:
        std::expected<std::vector<FrameDetections>, Error> detect(std::span<const Image> frames, std::stop_token) override {
            std::vector<FrameDetections> out(frames.size());
            for (FrameDetections& f : out) {
                f.faces = {Detection{Box{32, 36, 48, 72}, 0.9f, 0, {}}, Detection{Box{224, 36, 48, 72}, 0.9f, 0, {}}};
            }
            return out;
        }
        std::string modelVersion() const override { return "two/1"; }
    } detector;
    auto result = analyzeSubjects(talk, &detector, shotsOf(seconds(2)));
    REQUIRE(result);
    double left = 0.0;
    double right = 0.0;
    for (const SubjectSample& s : result->samples) {
        REQUIRE(s.faces.size() == 2);
        left += s.faces[0].box.x < 0.5f ? s.faces[0].activity : s.faces[1].activity;
        right += s.faces[0].box.x < 0.5f ? s.faces[1].activity : s.faces[0].activity;
    }
    CHECK(left > 0.05);
    CHECK(right == Approx(0.0).margin(1e-6));
}

TEST_CASE("Without a detector the salient region still guides; cancelling stops the analysis", "[ai][vision][subjects]") {
    FunctionVideo video(seconds(2), [](Ticks) { return canvas(160, 90, {Block{Box{0.7f, 0.3f, 0.12f, 0.3f}, 250, 250, 250}}, 30); });
    auto saliencyOnly = analyzeSubjects(video, nullptr, shotsOf(seconds(2)));
    REQUIRE(saliencyOnly);
    CHECK(saliencyOnly->models.empty());
    REQUIRE_FALSE(saliencyOnly->samples.empty());
    for (const SubjectSample& s : saliencyOnly->samples) {
        CHECK(s.faces.empty());
        REQUIRE(s.salient);
        CHECK(s.salient->cx() == Approx(0.76f).margin(0.1f));
    }

    BlockDetector detector;
    auto cancelled = analyzeSubjects(video, &detector, shotsOf(seconds(2)), {}, [](double p) { return p < 0.3; });
    REQUIRE_FALSE(cancelled);
    CHECK(cancelled.error().code == ErrorCode::Cancelled);

    std::stop_source stop;
    stop.request_stop();
    auto stopped = analyzeSubjects(video, &detector, shotsOf(seconds(2)), {}, {}, stop.get_token());
    REQUIRE_FALSE(stopped);
    CHECK(stopped.error().code == ErrorCode::Cancelled);

    // An empty range is an empty analysis; a failing source is an error.
    CHECK(analyzeSubjects(video, &detector, shotsOf(Ticks{0}))->samples.empty());
    class Broken : public TimedFrameSource {
        std::expected<Image, Error> frameAt(Ticks) override { return std::unexpected(Error(ErrorCode::DecoderError, "bad frame")); }
    } broken;
    CHECK(analyzeSubjects(broken, &detector, shotsOf(seconds(1))).error().code == ErrorCode::DecoderError);
}

// --- HelperDetector against the scripted helper -------------------------------------------------------------------------

namespace {

struct TempDir {
    explicit TempDir(const std::string& name) {
        std::random_device device;
        path = std::filesystem::temp_directory_path() / ("wabcut-vision-" + name + "-" + std::to_string(device()));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    std::filesystem::path path;
};

HelperDetectorOptions fakeDetector(const std::string& mode, std::vector<std::string> extra = {}) {
    HelperDetectorOptions o;
    o.executable = WABCUT_FAKE_EMBED;
    o.arguments = {"--mode", mode};
    o.arguments.insert(o.arguments.end(), extra.begin(), extra.end());
    o.faceModel = "yunet.onnx";
    o.objectModel = "yolox.onnx";
    o.modelVersion = "fake-detectors/1";
    o.batchSize = 2;
    o.startTimeout = std::chrono::seconds(10);
    o.loadTimeout = std::chrono::seconds(10);
    o.requestTimeout = std::chrono::seconds(10);
    return o;
}

std::vector<Image> scenes() {
    return {canvas(160, 90, {person(0.45f, 0.05f, 0.2f, 0.9f), face(0.5f, 0.1f, 0.1f, 0.2f)}),
            canvas(160, 90, {Block{Box{0.1f, 0.1f, 0.3f, 0.3f}, 0, 0, 255}}), // only a car
            canvas(160, 90, {}),
            canvas(64, 64, {face(0.25f, 0.25f, 0.5f, 0.5f)}),                     // another size: its own batch
            canvas(2048, 1152, {face(0.5f, 0.5f, 0.25f, 0.25f)})};                // beyond the protocol: shrunk, scaled back
}

} // namespace

TEST_CASE("HelperDetector finds faces and people through the helper", "[ai][vision][helper]") {
    HelperDetector detector(fakeDetector("normal"));
    CHECK(detector.modelVersion() == "fake-detectors/1");
    auto found = detector.detect(scenes(), {});
    REQUIRE(found);
    REQUIRE(found->size() == 5);
    const FrameDetections& first = (*found)[0];
    REQUIRE(first.faces.size() == 1);
    CHECK(first.faces[0].box.x == Approx(80).margin(1));
    CHECK(first.faces[0].box.w == Approx(16).margin(1));
    CHECK(first.faces[0].landmarks.size() == 10);
    CHECK(first.faces[0].score == Approx(0.9f));
    REQUIRE(first.people.size() == 1);
    CHECK(first.people[0].classId == kCocoPerson);
    CHECK((*found)[1].people.empty()); // the car is not a person
    CHECK((*found)[1].faces.empty());
    CHECK((*found)[2].faces.empty());
    CHECK((*found)[3].faces[0].box.x == Approx(16).margin(1));
    REQUIRE((*found)[4].faces.size() == 1);
    CHECK((*found)[4].faces[0].box.x == Approx(1024).margin(4)); // in the 2048-pixel picture's coordinates
    CHECK((*found)[4].faces[0].box.w == Approx(512).margin(4));
    CHECK(detector.device() == "FakeCPU");
    CHECK(detector.starts() == 1);
    CHECK(detector.inferenceMilliseconds() > 0.0);
    CHECK(detector.detect({}, {})->empty());

    // Through the pipeline: the detector drives a real analysis.
    FunctionVideo video(seconds(1), driftingFace);
    auto analysis = analyzeSubjects(video, &detector, shotsOf(seconds(1)));
    REQUIRE(analysis);
    CHECK(analysis->models == "fake-detectors/1");
    CHECK(std::ranges::all_of(analysis->samples, [](const SubjectSample& s) { return s.faces.size() == 1 && s.people.size() == 1; }));
    CHECK(detector.starts() == 1); // the same helper, its models still loaded

    detector.shutdown();
    REQUIRE(detector.detect(scenes(), {}));
    CHECK(detector.starts() == 2);

    auto none = fakeDetector("normal");
    none.faceModel.clear();
    none.objectModel.clear();
    CHECK(HelperDetector(none).detect(scenes(), {}).error().code == ErrorCode::InvalidArgument);
    CHECK(HelperDetector(fakeDetector("normal")).detect(std::vector<Image>{Image{}}, {}).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("HelperDetector turns helper crashes, hangs and bad answers into errors and recovers", "[ai][vision][helper]") {
    TempDir dir("faults");
    {
        HelperDetector crash(fakeDetector("crash-on-embed"));
        auto failed = crash.detect(scenes(), {});
        REQUIRE_FALSE(failed);
        CHECK(failed.error().code == ErrorCode::DecoderError);
        CHECK(failed.error().message.find("exit code 57") != std::string::npos);
    }
    {
        HelperDetector once(fakeDetector("crash-once", {"--marker", (dir.path / "crashed").string()}));
        CHECK_FALSE(once.detect(scenes(), {}));
        auto retried = once.detect(scenes(), {});
        REQUIRE(retried);
        CHECK(retried->size() == 5);
        CHECK(once.starts() == 2);
    }
    {
        auto options = fakeDetector("hang-on-embed");
        options.requestTimeout = std::chrono::milliseconds(800);
        HelperDetector hang(options);
        const auto started = std::chrono::steady_clock::now();
        auto failed = hang.detect(scenes(), {});
        REQUIRE_FALSE(failed);
        CHECK(failed.error().message.find("did not answer within") != std::string::npos);
        CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(8));
    }
    {
        HelperDetector hang(fakeDetector("hang-on-embed"));
        std::stop_source stop;
        std::thread canceller([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            stop.request_stop();
        });
        auto cancelled = hang.detect(scenes(), stop.get_token());
        canceller.join();
        REQUIRE_FALSE(cancelled);
        CHECK(cancelled.error().code == ErrorCode::Cancelled);
    }
    for (const auto& [mode, expected] : std::vector<std::pair<std::string, std::string>>{
             {"garbage", "invalid message"},
             {"wrong-row", "invalid message"},
             {"wrong-count", "detections of"},
             {"wrong-protocol", "protocol 99"},
             {"fail-load", "no such model file"},
             {"crash-at-start", "start"}}) {
        INFO(mode);
        HelperDetector broken(fakeDetector(mode));
        auto failed = broken.detect(scenes(), {});
        REQUIRE_FALSE(failed);
        CHECK(failed.error().message.find(expected) != std::string::npos);
    }
    auto missing = fakeDetector("normal");
    missing.executable = dir.path / "no-such-helper.exe";
    CHECK(HelperDetector(missing).detect(scenes(), {}).error().code == ErrorCode::NotFound);
}

TEST_CASE("The built-in detectors are pinned, licensed and versioned", "[ai][vision][models]") {
    const auto files = subjectModelFiles();
    REQUIRE(files.size() == 2);
    for (const ai::ModelInfo* m : files) {
        REQUIRE(m != nullptr);
        CHECK(m->kind == ai::ModelKind::Detection);
        CHECK(m->sha256.size() == 64);
        CHECK(m->url.starts_with("https://"));
        CHECK(m->url.ends_with(m->fileName));
        CHECK(m->url.find("/main/") == std::string::npos); // pinned, not a branch
        CHECK_FALSE(m->licenseCard.empty());
    }
    CHECK(files[0]->license == "MIT");        // YuNet
    CHECK(files[1]->license == "Apache-2.0"); // YOLOX
    CHECK(subjectModelVersion() == "yunet-2023mar-8f2383e4+yolox-tiny-427cc366/v1");
    TempDir dir("store");
    const ai::ModelStore store(dir.path);
    CHECK_FALSE(subjectModelsInstalled(store));
    const HelperDetectorOptions options = subjectDetectorOptions(store);
    CHECK(options.faceModel.filename() == "face_detection_yunet_2023mar.onnx");
    CHECK(options.objectModel.filename() == "yolox_tiny.onnx");
}
