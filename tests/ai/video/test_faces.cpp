// AI Face Retouch (card 132) on the AI video runner: the two face models are verified, pinned downloads (the parser brings its
// detector); the crop round a face, the parser's input and the class scores' softmax become skin, mouth and eye coverage and are
// pasted into a map of the picture with a soft edge; the runner makes a face map per frame at the working size, stores it as three
// 8-bit planes and skips what the cache has; a clip's face target is shared by every Face Retouch; HelperBackend runs the parser
// with its detector through the scripted helper. The opt-in test at the end runs the real models on a photo of a face
// (WABCUT_TEST_VIDEO_MODELS_DIR, WABCUT_TEST_FACE_IMAGE). Fake backend and frame source: no model, no media.

#include "VideoTestUtil.h"

#include <wabcut/ai/ModelStore.h>
#include <wabcut/ai/video/FaceParse.h>
#include <wabcut/ai/video/HelperBackend.h>
#include <wabcut/ai/video/ModelKeys.h>
#include <wabcut/ai/video/Picture.h>
#include <wabcut/ai/video/Runner.h>
#include <wabcut/ai/video/Targets.h>
#include <wabcut/ai/video/VideoModels.h>
#include <wabcut/model/Clip.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/model/Sequence.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <limits>
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

constexpr int kSize = kFaceParseSize;
constexpr std::size_t kPlane = static_cast<std::size_t>(kSize) * static_cast<std::size_t>(kSize);

/// Logits for 19 classes where every pixel's class is chosen by `classAt` with a score of 12 against 0 for the others.
std::vector<float> logitsOf(const std::function<int(int, int)>& classAt) {
    std::vector<float> logits(kPlane * 19, 0.0f);
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            logits[static_cast<std::size_t>(classAt(x, y)) * kPlane + static_cast<std::size_t>(y) * kSize + static_cast<std::size_t>(x)] = 12.0f;
        }
    }
    return logits;
}

Job faceJob(int frames) {
    Job job;
    job.kind = JobKind::Faces;
    job.chain = aiFaceChain();
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

TEST_CASE("The face models are verified pinned downloads and the parser brings its detector", "[ai][video][faces][models]") {
    const ModelInfo* parser = findModel("video-face-parsing-bisenet");
    REQUIRE(parser != nullptr);
    CHECK(parser->kind == ModelKind::Video);
    CHECK(parser->license == "MIT");
    CHECK(isAllowedModelLicense(parser->license));
    CHECK_FALSE(requiresLicenseOptIn(*parser));
    CHECK(parser->size == 53'205'364);
    CHECK(parser->sha256 == "0d9bd318e46987c3bdbfacae9e2c0f461cae1c6ac6ea6d43bbe541a91727e33f");
    // A release asset of a version tag of the author's repository, never a branch: the digest pins the bytes either way.
    CHECK(parser->url == "https://github.com/yakhyo/face-parsing/releases/download/v0.0.2/resnet18.onnx");
    CHECK(parser->fileName.starts_with("ai-video/"));
    CHECK(parser->licenseCard.find("MIT License") != std::string::npos);
    CHECK(parser->licenseCard.find("CelebAMask-HQ") != std::string::npos); // where the weights come from is said before the download
    CHECK(parser->licenseCard.find("https://") != std::string::npos);
    const ModelInfo* detector = findModel("yunet-2023mar");
    REQUIRE(detector != nullptr);
    CHECK(detector->license == "MIT");
    CHECK(describe(Model::FaceParse).key == kKeyFaceParse);
    CHECK(describe(Model::FaceDetect).key == kKeyFaceDetect);
    CHECK(describe(Model::FaceParse).scale == 1);
    // The chain names both, the detector first, and both are in its cache's version.
    const AiChain chain = aiFaceChain();
    REQUIRE(stepsOf(chain).size() == 1);
    CHECK(stepsOf(chain).front().model == Model::FaceParse);
    const std::vector<Model> models = modelsOf(chain);
    REQUIRE(models.size() == 2);
    CHECK(models[0] == Model::FaceDetect);
    CHECK(models[1] == Model::FaceParse);
    CHECK(modelVersion(chain) == "yunet-face-detect-8f2383e4+bisenet-face-parse-0d9bd318-p1");
    CHECK(aiChainHash(chain) != 0);
    CHECK(aiChainHash(chain) != aiChainHash(aiDepthChain()));
    CHECK(aiChainLabel(chain) == "Face maps");
    CHECK(chain.stages.front().isSideProduct());
    CHECK_FALSE(chain.stages.front().isNoop());
    // Nothing is installed in an empty store: the download is both models, the card shows both licences.
    ModelStore empty(freshDir("faces-store"));
    CHECK_FALSE(modelsInstalled(chain, empty));
    CHECK(missingBytes(chain, empty) == parser->size + detector->size);
    const std::string card = downloadCard(chain, empty);
    CHECK(card.find("YuNet") != std::string::npos);
    CHECK(card.find("BiSeNet") != std::string::npos);
    const std::string text = missingModelsText(chain, empty);
    CHECK(text.find("YuNet face detector") != std::string::npos);
    CHECK(text.find("BiSeNet face parsing (53 MB)") != std::string::npos);
}

TEST_CASE("The face models look at the picture at most 1280 pixels on the long side", "[ai][video][faces]") {
    int w = 0;
    int h = 0;
    faceWorkingSize(1920, 1080, w, h);
    CHECK(w == 1280);
    CHECK(h == 720);
    faceWorkingSize(3840, 2160, w, h);
    CHECK(w == 1280);
    CHECK(h == 720);
    faceWorkingSize(1080, 1920, w, h); // a phone clip
    CHECK(w == 720);
    CHECK(h == 1280);
    faceWorkingSize(640, 360, w, h); // never enlarged
    CHECK(w == 640);
    CHECK(h == 360);
    faceWorkingSize(1, 1, w, h);
    CHECK(w == 2);
    CHECK(h == 2);
    // The estimate follows the working picture, not the frame: a 4K frame costs what a 720p one does.
    const AiChain chain = aiFaceChain();
    CHECK(estimateFrameMilliseconds(chain, 3840, 2160, true) == Approx(estimateFrameMilliseconds(chain, 1280, 720, true)));
}

TEST_CASE("A face crop is a square round the box with room for the forehead and a floor of 96 pixels", "[ai][video][faces]") {
    const FaceCrop crop = faceCropFor({400.0, 200.0, 152.0, 190.0});
    CHECK(crop.side == Approx(285.0)); // 1.5 x the larger side
    CHECK(crop.cx == Approx(476.0));
    CHECK(crop.cy == Approx(295.0 - 0.03 * 285.0)); // the box's centre, a little higher
    const FaceCrop tiny = faceCropFor({10.0, 10.0, 20.0, 24.0});
    CHECK(tiny.side == Approx(96.0));
}

TEST_CASE("The parser's input is the crop, normalised with the ImageNet mean and deviation", "[ai][video][faces]") {
    // A flat picture: every sample is the same value, edge pixels repeat outside the picture.
    Picture flat;
    flat.width = 40;
    flat.height = 30;
    flat.rgb.assign(flat.pixels() * 3, 0);
    for (std::size_t i = 0; i < flat.pixels(); ++i) {
        flat.rgb[3 * i] = 65535;
        flat.rgb[3 * i + 1] = 32768;
        flat.rgb[3 * i + 2] = 0;
    }
    const FaceCrop reaching{30.0, 20.0, 100.0}; // half of it outside the picture
    const std::vector<float> input = faceParseInput(flat, reaching);
    REQUIRE(input.size() == kPlane * 3);
    CHECK(input.front() == Approx((1.0f - 0.485f) / 0.229f).margin(1e-4));
    CHECK(input[kPlane] == Approx((0.5f - 0.456f) / 0.224f).margin(1e-3));
    CHECK(input[2 * kPlane] == Approx((0.0f - 0.406f) / 0.225f).margin(1e-4));
    CHECK(input.back() == Approx((0.0f - 0.406f) / 0.225f).margin(1e-4)); // the last blue sample: the crop's bottom right corner
    // A horizontal ramp: the crop sees it left to right, the other way round it would be reversed.
    Picture ramp;
    ramp.width = 200;
    ramp.height = 100;
    ramp.rgb.resize(ramp.pixels() * 3);
    for (int y = 0; y < ramp.height; ++y) {
        for (int x = 0; x < ramp.width; ++x) {
            const auto v = static_cast<std::uint16_t>(65535.0 * x / (ramp.width - 1));
            for (int c = 0; c < 3; ++c) {
                ramp.rgb[(static_cast<std::size_t>(y) * ramp.width + x) * 3 + static_cast<std::size_t>(c)] = v;
            }
        }
    }
    const std::vector<float> seen = faceParseInput(ramp, {100.0, 50.0, 100.0});
    CHECK(seen[0] < seen[kSize - 1]);
    CHECK(seen[kSize / 2] == Approx((0.5f - 0.485f) / 0.229f).margin(0.02)); // the middle of the crop is the middle of the ramp
    CHECK(seen[kSize / 2] == Approx(seen[kPlane * 0 + kSize * 100 + kSize / 2]).margin(1e-5)); // the same on every row
    // Nothing to look at: an invalid picture or an empty crop.
    CHECK(faceParseInput(Picture{}, reaching).empty());
    CHECK(faceParseInput(flat, {10.0, 10.0, 0.0}).empty());
}

TEST_CASE("Class scores become skin, mouth and eye coverage", "[ai][video][faces]") {
    // Left third skin (1), then nose (10), ears (7 and 8), mouth (11), eyes (4 and 5), hair (17), neck (14), glasses (6), lips (12) and
    // background (0) in bands of 24 columns.
    constexpr int kClassOfBand[] = {1, 10, 7, 8, 11, 4, 5, 17, 14, 6, 12, 0, 2, 16};
    const std::vector<float> logits = logitsOf([&](int x, int) { return kClassOfBand[std::min(x / 24, 13)]; });
    const FaceClasses classes = faceClassesOf(logits);
    REQUIRE(classes.skin.size() == kPlane);
    const auto at = [&](const std::vector<std::uint8_t>& plane, int band) { return plane[static_cast<std::size_t>(100) * kSize + static_cast<std::size_t>(band * 24 + 12)]; };
    for (int band : {0, 1, 2, 3}) {
        CHECK(at(classes.skin, band) == 255); // skin, nose, both ears
        CHECK(at(classes.mouth, band) == 0);
        CHECK(at(classes.eyes, band) == 0);
    }
    CHECK(at(classes.mouth, 4) == 255);
    CHECK(at(classes.skin, 4) == 0);
    CHECK(at(classes.eyes, 5) == 255);
    CHECK(at(classes.eyes, 6) == 255);
    CHECK(at(classes.skin, 5) == 0);
    // Hair, glasses, lips, background, brows and cloth are in none of the three.
    for (int band : {7, 9, 10, 11, 12, 13}) {
        INFO(band);
        CHECK(at(classes.skin, band) == 0);
        CHECK(at(classes.mouth, band) == 0);
        CHECK(at(classes.eyes, band) == 0);
    }
    // The neck counts a little less than the face, so the jaw blends.
    CHECK(at(classes.skin, 8) == 204); // 0.8
    // Two classes of equal score share the pixel: skin against background is half skin.
    std::vector<float> tie(kPlane * 19, -20.0f);
    for (std::size_t i = 0; i < kPlane; ++i) {
        tie[kPlane * 1 + i] = 3.0f;
        tie[i] = 3.0f; // class 0 background
    }
    const FaceClasses half = faceClassesOf(tie);
    CHECK(half.skin[1000] == Approx(128).margin(1));
    CHECK(half.mouth[1000] == 0);
    // A wrong-sized output and non-finite scores give nothing usable / nothing wrong.
    CHECK(faceClassesOf(std::vector<float>(10)).skin.empty());
    std::vector<float> broken = logits;
    broken[5] = std::numeric_limits<float>::quiet_NaN();
    broken[kPlane + 7] = std::numeric_limits<float>::infinity();
    const FaceClasses safe = faceClassesOf(broken);
    REQUIRE(safe.skin.size() == kPlane);
    CHECK(safe.skin[kSize * 100 + 3] == 255);
}

TEST_CASE("A face is pasted into the map with a soft edge and the larger value wins", "[ai][video][faces]") {
    FaceMapBytes map(300, 200);
    FaceClasses face;
    face.skin.assign(kPlane, 255);
    face.mouth.assign(kPlane, 0);
    face.eyes.assign(kPlane, 0);
    for (int y = 240; y < 280; ++y) {
        for (int x = 200; x < 300; ++x) {
            face.mouth[static_cast<std::size_t>(y) * kSize + static_cast<std::size_t>(x)] = 200;
        }
    }
    const FaceCrop crop{150.0, 100.0, 128.0}; // spans 86..214 x 36..164: four plane texels to a map pixel
    pasteFace(map, face, crop);
    const auto at = [&](int x, int y, int channel) { return map.coverage[(static_cast<std::size_t>(y) * 300 + static_cast<std::size_t>(x)) * 3 + static_cast<std::size_t>(channel)]; };
    CHECK(at(150, 100, 0) == 255);            // the middle of the crop
    CHECK(at(150, 110, 1) == 0);              // below the mouth bar
    CHECK(at(20, 20, 0) == 0);                // outside the crop (it spans 86..214 x 36..164)
    CHECK(at(250, 100, 0) == 0);
    // The edge fades: the pixel next to the crop's border is far from full, the one a few pixels in is less so.
    CHECK(at(86, 100, 0) < 40);
    CHECK(at(86, 100, 0) < at(92, 100, 0));
    CHECK(at(92, 100, 0) <= at(100, 100, 0));
    CHECK(at(100, 100, 0) <= 255);
    // The mouth bar sits where the plane's rows 240..280 and columns 200..300 are: map y = 36 + 240 / 4 = 96 .. 106.
    CHECK(at(150, 101, 1) == 200);
    CHECK(at(150, 90, 1) == 0);
    // A second face over the first keeps the larger of the two values.
    FaceClasses weaker;
    weaker.skin.assign(kPlane, 100);
    weaker.mouth.assign(kPlane, 250);
    weaker.eyes.assign(kPlane, 0);
    pasteFace(map, weaker, crop);
    CHECK(at(150, 100, 0) == 255);
    CHECK(at(150, 101, 1) == 250);
    // A crop partly outside the map, a crop with no side, a map that is not valid and planes of the wrong size: nothing is written
    // outside, nothing crashes.
    FaceMapBytes edge(60, 40);
    pasteFace(edge, face, {0.0, 0.0, 200.0});
    CHECK(edge.coverage[(5 * 60 + 5) * 3] == 255);
    FaceMapBytes untouched(20, 20);
    pasteFace(untouched, face, {10.0, 10.0, 0.0});
    FaceMapBytes invalid;
    pasteFace(invalid, face, crop);
    FaceClasses wrong;
    wrong.skin.assign(10, 255);
    pasteFace(untouched, wrong, {10.0, 10.0, 100.0});
    CHECK(std::all_of(untouched.coverage.begin(), untouched.coverage.end(), [](std::uint8_t v) { return v == 0; }));
}

TEST_CASE("A face map picture round trips every byte", "[ai][video][faces]") {
    FaceMapBytes map(256, 3);
    for (int x = 0; x < 256; ++x) {
        for (int y = 0; y < 3; ++y) {
            for (int c = 0; c < 3; ++c) {
                map.coverage[(static_cast<std::size_t>(y) * 256 + static_cast<std::size_t>(x)) * 3 + static_cast<std::size_t>(c)] =
                    static_cast<std::uint8_t>((x + y * 85 + c * 40) % 256);
            }
        }
    }
    const Picture picture = faceMapPicture(map);
    REQUIRE(picture.valid());
    CHECK(picture.rgb[3 * 255] == 65535);
    const FaceMapBytes back = faceMapFromPicture(picture);
    REQUIRE(back.valid());
    CHECK(back.width == 256);
    CHECK(back.coverage == map.coverage);
    CHECK_FALSE(faceMapPicture(FaceMapBytes{}).valid());
    CHECK_FALSE(faceMapFromPicture(Picture{}).valid());
}

TEST_CASE("A face chain runs the parser at the working size and gives the map at that size", "[ai][video][faces]") {
    FakeBackend backend;
    const auto made = processPicture(pattern(2560, 1440), aiFaceChain(), backend, nullptr, {});
    REQUIRE(made);
    REQUIRE(backend.calls.size() == 1);
    CHECK(backend.calls[0].model == Model::FaceParse);
    CHECK(backend.calls[0].width == 1280);
    CHECK(backend.calls[0].height == 720);
    CHECK(made->width == 1280);
    CHECK(made->height == 720);
    CHECK(made->rgb == fakeFaceMap(1280, 720).rgb);
    // Alpha is not part of a face map; a smaller picture is looked at as it is.
    FakeBackend second;
    const auto small = processPicture(pattern(320, 180, 0, true), aiFaceChain(), second, nullptr, {});
    REQUIRE(small);
    CHECK(second.calls[0].width == 320);
    CHECK_FALSE(small->hasAlpha());
    // A backend that answers with the wrong size is refused, not stored.
    class WrongSize final : public IVideoBackend {
    public:
        std::expected<StepResult, Error> run(const Picture&, const Step&, std::stop_token) override {
            StepResult r;
            r.picture = fakeFaceMap(10, 10);
            return r;
        }
    } wrong;
    const auto refused = processPicture(pattern(64, 36), aiFaceChain(), wrong, nullptr, {});
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == ErrorCode::DecoderError);
}

TEST_CASE("A face job stores three 8-bit planes per frame and skips what the cache has", "[ai][video][faces]") {
    ProcessedCache cache(freshDir("faces-job"));
    FakeBackend backend;
    FakeSource source(3, 320, 180);
    Runner runner(source, backend, cache);
    const Job job = faceJob(3);
    const auto first = runner.run(job, {}, {});
    REQUIRE(first);
    CHECK(first->processed == 3);
    CHECK(backend.calls.size() == 3);
    CHECK(cache.frames(job.identity).size() == 3);
    const auto stored = cache.get(job.identity, frameTicks(1), Ticks{frameTicks(1).value / 2});
    REQUIRE(stored);
    REQUIRE(stored->planes.size() == 3);
    const Picture expected = fakeFaceMap(320, 180);
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const StoredPlane& plane = stored->planes[channel];
        CHECK(plane.bytesPerTexel == 1);
        CHECK(plane.width == 320);
        CHECK(plane.height == 180);
        REQUIRE(plane.data.size() == 320u * 180u);
        for (std::size_t i = 0; i < plane.data.size(); i += 97) {
            REQUIRE(plane.data[i] == expected.rgb[3 * i + channel] / 257);
        }
    }
    CHECK(stored->planes[0].data[90 * 320 + 160] == 255); // skin in the middle
    CHECK(stored->planes[1].data[90 * 320 + 160] == 0);
    // Run again: nothing is made.
    const auto again = runner.run(job, {}, {});
    REQUIRE(again);
    CHECK(again->processed == 0);
    CHECK(again->skipped == 3);
    CHECK(backend.calls.size() == 3);
    // A map the model got wrong (an empty answer) is an error, and nothing half-made is stored.
    CHECK_FALSE(faceStoredFrame(Picture{}, Ticks{}, Ticks{}).has_value());
    const auto ok = faceStoredFrame(expected, frameTicks(2), frameTicks(1));
    REQUIRE(ok);
    CHECK(ok->pts == frameTicks(2).value);
    CHECK(ok->duration == frameTicks(1).value);
    CHECK(ok->planes.size() == 3);
}

TEST_CASE("A clip with AI Face Retouch has a face target shared by every Face Retouch; a disabled one has none", "[ai][video][faces][targets]") {
    ProjectState s;
    auto media = mediaOf(s, 960, 540);
    s.media = {media};
    const auto retouch = [&](bool enabled) {
        EffectInstance e;
        e.id = s.ids.allocate<EffectInstanceId>();
        e.effect = std::string(kAiFaceRetouchEffect);
        e.enabled = enabled;
        return e;
    };
    Clip clip;
    clip.id = s.ids.allocate<ClipId>();
    clip.media = media->id;
    clip.timelineRange = {Ticks{}, Ticks::fromSeconds(2)};
    clip.setEffects({retouch(true), retouch(true)});
    const Sequence seq = sequenceOf(s, {clip});
    const Clip& placed = seq.videoTracks.front()->clips.front();
    const auto t = faceTarget(s, seq, placed, 0);
    REQUIRE(t.has_value());
    CHECK(t->kind == JobKind::Faces);
    CHECK(t->chain.stages.size() == 1); // two Face Retouch effects, one set of face maps
    CHECK(t->expected == 50);
    CHECK(t->identity == identityOf(*media, 0, aiFaceChain()));
    CHECK(t->identity != identityOf(*media, 0, aiDepthChain())); // their pictures never share a folder
    CHECK(aiChainLabel(t->chain) == "Face maps");
    // It is not a source-stage effect: Restore and Denoise ignore it, and it makes no depth maps.
    const auto pictures = clipTarget(s, seq, placed, 0);
    REQUIRE(pictures.has_value());
    CHECK(pictures->chain.empty());
    CHECK_FALSE(depthTarget(s, seq, placed, 0).has_value());
    // A disabled Face Retouch makes no face maps.
    Clip off = placed;
    off.setEffects({retouch(false)});
    CHECK_FALSE(faceTarget(s, seq, off, 0).has_value());
    // The sequence lists the face target (and only it); with AI Refocus beside it the clip has both, each its own job.
    const auto all = sequenceTargets(s, seq, std::nullopt);
    REQUIRE(all.size() == 1);
    CHECK(all[0].kind == JobKind::Faces);
    Clip both = placed;
    EffectInstance focus;
    focus.id = s.ids.allocate<EffectInstanceId>();
    focus.effect = std::string(kAiRefocusEffect);
    both.setEffects({retouch(true), focus});
    const Sequence seq2 = sequenceOf(s, {both});
    const auto twice = sequenceTargets(s, seq2, std::nullopt);
    REQUIRE(twice.size() == 2);
    std::set<JobKind> kinds{twice[0].kind, twice[1].kind};
    CHECK(kinds == std::set<JobKind>{JobKind::Depth, JobKind::Faces});
    // How much of it is made: counted from the cache, frame by frame.
    ProcessedCache cache(freshDir("faces-targets"));
    CHECK(coveredCount(cache, *t) == 0);
    FakeBackend backend;
    FakeSource source(3, 64, 36);
    Runner runner(source, backend, cache);
    Job job = faceJob(3);
    job.identity = t->identity;
    REQUIRE(runner.run(job, {}, {}));
    CHECK(coveredCount(cache, *t) == 3);
}

TEST_CASE("HelperBackend makes a face map through the scripted helper and loads the detector with the parser", "[ai][video][faces][helper]") {
    std::set<Model> resolved;
    HelperBackendOptions o;
    o.executable = WABCUT_FAKE_EMBED;
    o.arguments = {"--mode", "normal"};
    o.workDir = freshDir("faces-helper");
    o.resolveModel = [&](Model model) {
        resolved.insert(model);
        return std::optional<std::filesystem::path>{"fake-model.onnx"};
    };
    o.startTimeout = std::chrono::seconds(20);
    o.loadTimeout = std::chrono::seconds(20);
    o.requestBase = std::chrono::seconds(20);
    HelperBackend backend(o);
    Step step;
    step.model = Model::FaceParse;
    const auto made = backend.run(pattern(64, 48), step, {});
    REQUIRE(made);
    CHECK(resolved == std::set<Model>{Model::FaceParse, Model::FaceDetect});
    CHECK(made->picture.width == 64);
    CHECK(made->picture.height == 48);
    const Picture expected = fakeFaceMap(64, 48);
    CHECK(made->picture.rgb == expected.rgb);
    const FaceMapBytes map = faceMapFromPicture(made->picture);
    const auto at = [&](int x, int y, int channel) { return map.coverage[(static_cast<std::size_t>(y) * 64 + static_cast<std::size_t>(x)) * 3 + static_cast<std::size_t>(channel)]; };
    CHECK(at(32, 24, 0) == 255); // skin in the middle
    CHECK(at(32, 31, 1) == 255); // the mouth bar
    CHECK(at(24, 19, 2) == 255); // an eye
    CHECK(at(2, 2, 0) == 0);     // the background
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

struct Region {
    long long count = 0;
    double meanY = 0.0;
    int left = 1 << 30, right = -1, top = 1 << 30, bottom = -1;
};

Region regionOf(const FaceMapBytes& map, int channel, std::uint8_t threshold = 128) {
    Region r;
    for (int y = 0; y < map.height; ++y) {
        for (int x = 0; x < map.width; ++x) {
            if (map.coverage[(static_cast<std::size_t>(y) * static_cast<std::size_t>(map.width) + static_cast<std::size_t>(x)) * 3 + static_cast<std::size_t>(channel)] >= threshold) {
                ++r.count;
                r.meanY += y;
                r.left = std::min(r.left, x);
                r.right = std::max(r.right, x);
                r.top = std::min(r.top, y);
                r.bottom = std::max(r.bottom, y);
            }
        }
    }
    r.meanY = r.count > 0 ? r.meanY / static_cast<double>(r.count) : 0.0;
    return r;
}

} // namespace

TEST_CASE("The real face models: YuNet finds the face and BiSeNet maps its skin, mouth and eyes (WABCUT_TEST_VIDEO_MODELS_DIR, WABCUT_TEST_FACE_IMAGE)",
          "[ai][video][faces][live]") {
    const char* dir = std::getenv("WABCUT_TEST_VIDEO_MODELS_DIR");
    const char* image = std::getenv("WABCUT_TEST_FACE_IMAGE");
    if (dir == nullptr || *dir == '\0' || image == nullptr || *image == '\0') {
        WARN("skipped: set WABCUT_TEST_VIDEO_MODELS_DIR (face_parsing_resnet18.onnx, face_detection_yunet_2023mar.onnx) and WABCUT_TEST_FACE_IMAGE (a P6 PPM of a face)");
        return;
    }
    const std::filesystem::path parserFile = std::filesystem::path(dir) / "face_parsing_resnet18.onnx";
    const std::filesystem::path detectorFile = std::filesystem::path(dir) / "face_detection_yunet_2023mar.onnx";
    REQUIRE(std::filesystem::is_regular_file(parserFile));
    REQUIRE(std::filesystem::is_regular_file(detectorFile));
    const auto photo = readPpm(image);
    REQUIRE(photo.has_value());
    Step step;
    step.model = Model::FaceParse;
    std::vector<FaceMapBytes> maps;
    for (const char* device : {"auto", "cpu"}) {
        HelperBackendOptions o;
        o.executable = WABCUT_EMBED_HELPER;
        o.device = device;
        o.workDir = freshDir(std::string("faces-live-") + device);
        o.resolveModel = [&](Model model) { return std::optional<std::filesystem::path>{model == Model::FaceParse ? parserFile : detectorFile}; };
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
        REQUIRE(second->picture.width == photo->width);
        REQUIRE(second->picture.height == photo->height);
        FaceMapBytes map = faceMapFromPicture(second->picture);
        REQUIRE(map.valid());
        const Region skin = regionOf(map, 0);
        const Region mouth = regionOf(map, 1);
        const Region eyes = regionOf(map, 2);
        WARN(device << ": skin " << skin.count << " px, mouth " << mouth.count << " px, eyes " << eyes.count << " px");
        CHECK(skin.count > 5'000);
        CHECK(skin.count < static_cast<long long>(photo->pixels()) / 2);
        CHECK(mouth.count > 200);
        CHECK(eyes.count > 80);
        CHECK(eyes.meanY < mouth.meanY);                                  // the eyes are above the mouth
        CHECK(mouth.left >= skin.left);                                   // the mouth is inside the face
        CHECK(mouth.right <= skin.right);
        CHECK(mouth.top >= skin.top);
        CHECK(mouth.bottom <= skin.bottom);
        CHECK(eyes.top >= skin.top);
        // The regions do not overlap: a pixel is skin or mouth or eye (the sum of the three never exceeds 1.1).
        long long overlap = 0;
        for (std::size_t i = 0; i < map.coverage.size(); i += 3) {
            overlap += map.coverage[i] + map.coverage[i + 1] + map.coverage[i + 2] > 280 ? 1 : 0;
        }
        CHECK(overlap < skin.count / 50);
        // The picture with the map over it (skin green, mouth red, eyes blue), for looking at: test-output/ai-video/faces-overlay-<device>.ppm.
        {
            std::filesystem::create_directories(std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "ai-video");
            std::ofstream out(std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "ai-video" / (std::string("faces-overlay-") + device + ".ppm"), std::ios::binary);
            out << "P6\n" << map.width << " " << map.height << "\n255\n";
            for (std::size_t i = 0; i < photo->pixels(); ++i) {
                const double skinK = map.coverage[3 * i] / 255.0;
                const double mouthK = map.coverage[3 * i + 1] / 255.0;
                const double eyeK = map.coverage[3 * i + 2] / 255.0;
                const double r = photo->rgb[3 * i] / 257.0 * 0.45 + 255.0 * 0.7 * mouthK;
                const double g = photo->rgb[3 * i + 1] / 257.0 * 0.45 + 160.0 * skinK;
                const double b = photo->rgb[3 * i + 2] / 257.0 * 0.45 + 255.0 * 0.7 * eyeK;
                const char px[3] = {static_cast<char>(std::min(255.0, r)), static_cast<char>(std::min(255.0, g)), static_cast<char>(std::min(255.0, b))};
                out.write(px, 3);
            }
        }
        if (std::string(device) == "auto") {
            // The map itself as a P6 picture (R skin, G mouth, B eyes), for the render suite's opt-in test on the same photo.
            std::ofstream out(std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "ai-video" / "faces-map.ppm", std::ios::binary);
            out << "P6\n" << map.width << " " << map.height << "\n255\n";
            out.write(reinterpret_cast<const char*>(map.coverage.data()), static_cast<std::streamsize>(map.coverage.size()));
        }
        maps.push_back(std::move(map));
    }
    // The GPU and the CPU agree on nearly every pixel.
    REQUIRE(maps.size() == 2);
    long long differ = 0;
    long long used = 0;
    for (std::size_t i = 0; i < maps[0].coverage.size(); ++i) {
        const int d = std::abs(static_cast<int>(maps[0].coverage[i]) - static_cast<int>(maps[1].coverage[i]));
        used += maps[0].coverage[i] > 0 || maps[1].coverage[i] > 0 ? 1 : 0;
        differ += d > 64 ? 1 : 0;
    }
    WARN("GPU and CPU differ by more than 25 % on " << differ << " of " << used << " covered values");
    CHECK(differ < used / 20 + 10);
}
#endif
