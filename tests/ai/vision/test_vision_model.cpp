// The real detectors (AI-9), optional: YuNet and YOLOX-Tiny in wabcut-embed on a photo and a 1080p video, on the CPU and
// on the GPU (DirectML), with the analysis throughput printed for the perf report. Never downloads anything:
//
//   WABCUT_TEST_DETECT_DIR    folder with face_detection_yunet_2023mar.onnx and yolox_tiny.onnx (checked against the
//                             manifest's SHA-256)
//   WABCUT_TEST_DETECT_IMAGE  a binary PPM (P6) photo with several faces and people
//   WABCUT_TEST_SUBJECT_VIDEO a 1080p video for the throughput figure (full builds)

#include <wabcut/ai/ModelStore.h>
#include <wabcut/ai/Sha256.h>
#include <wabcut/ai/vision/HelperDetector.h>
#ifdef WABCUT_TEST_VISION_MEDIA
#include <wabcut/ai/ChildProcess.h>
#include <wabcut/ai/reframe/Reframe.h>
#include <wabcut/ai/vision/MediaSubjects.h>
#include <wabcut/media/Probe.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>
#endif

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::ai::vision;

// The helpers serve the tests that run with the wabcut-embed helper (WABCUT_EMBED_HELPER); the core build has none.
namespace {

[[maybe_unused]] std::optional<std::filesystem::path> envPath(const char* name) {
    const char* value = std::getenv(name);
    if (!value || !*value) {
        return std::nullopt;
    }
    return std::filesystem::path(value);
}

/// A binary PPM (P6, 8-bit) as an Image.
[[maybe_unused]] std::optional<Image> readPpm(const std::filesystem::path& file) {
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
    Image image;
    image.width = width;
    image.height = height;
    image.rgb.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3);
    in.read(reinterpret_cast<char*>(image.rgb.data()), static_cast<std::streamsize>(image.rgb.size()));
    return in ? std::optional<Image>(std::move(image)) : std::nullopt;
}

[[maybe_unused]] std::optional<std::string> sha256Of(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    ai::Sha256 hash;
    std::vector<char> buffer(1 << 16);
    while (in.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || in.gcount() > 0) {
        hash.update(std::string_view(buffer.data(), static_cast<std::size_t>(in.gcount())));
    }
    return hash.finishHex();
}

[[maybe_unused]] HelperDetectorOptions realDetector(const std::filesystem::path& dir, const std::string& device) {
    HelperDetectorOptions o;
#ifdef WABCUT_EMBED_HELPER
    o.executable = WABCUT_EMBED_HELPER;
#endif
    o.faceModel = dir / "face_detection_yunet_2023mar.onnx";
    o.objectModel = dir / "yolox_tiny.onnx";
    o.modelVersion = subjectModelVersion();
    o.device = device;
    o.batchSize = 8;
    return o;
}

} // namespace

TEST_CASE("YuNet and YOLOX in wabcut-embed find the faces and people of a photo (WABCUT_TEST_DETECT_DIR)", "[ai][vision][model]") {
#ifndef WABCUT_EMBED_HELPER
    SKIP("built without WABCUT_WITH_ONNX: no wabcut-embed");
#else
    const auto dir = envPath("WABCUT_TEST_DETECT_DIR");
    const auto imageFile = envPath("WABCUT_TEST_DETECT_IMAGE");
    if (!dir || !imageFile) {
        SKIP("set WABCUT_TEST_DETECT_DIR (the two .onnx files) and WABCUT_TEST_DETECT_IMAGE (a P6 photo with faces)");
    }
    // The files are the manifest's, byte for byte.
    for (const ai::ModelInfo* m : subjectModelFiles()) {
        REQUIRE(m != nullptr);
        const auto digest = sha256Of(*dir / m->fileName);
        REQUIRE(digest);
        CHECK(*digest == m->sha256);
    }
    const auto photo = readPpm(*imageFile);
    REQUIRE(photo);
    for (const std::string device : {"cpu", "auto"}) {
        INFO(device);
        HelperDetector detector(realDetector(*dir, device));
        const std::vector<Image> frames(8, *photo);
        auto warm = detector.detect(std::span(frames).first(1), {}); // loads the models
        REQUIRE(warm);
        const auto started = std::chrono::steady_clock::now();
        auto found = detector.detect(frames, {});
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        REQUIRE(found);
        REQUIRE(found->size() == 8);
        const FrameDetections& first = (*found)[0];
        std::printf("[perf] YuNet + YOLOX-Tiny on %s (%s): %zu faces, %zu people in a %dx%d photo; %.1f pictures/s (helper %.1f ms each)\n",
                    detector.device().c_str(), device.c_str(), first.faces.size(), first.people.size(), photo->width, photo->height,
                    8.0 / seconds, detector.inferenceMilliseconds() / 9.0);
        CHECK(first.faces.size() >= 5);
        CHECK(first.people.size() >= 3);
        for (const Detection& face : first.faces) {
            CHECK(face.box.x >= -1.0f);
            CHECK(face.box.x + face.box.w <= static_cast<float>(photo->width) + 1.0f);
            CHECK(face.score >= 0.6f);
            REQUIRE(face.landmarks.size() == 10);
            // The eyes lie inside their face, in its upper half.
            CHECK(face.landmarks[0] >= face.box.x - 2.0f);
            CHECK(face.landmarks[1] <= face.box.y + face.box.h * 0.7f);
        }
        CHECK((*found)[7].faces.size() == first.faces.size()); // the same picture, the same answer
        if (device == "auto") {
            CHECK_FALSE(detector.device().empty());
        }
    }
#endif
}

#if defined(WABCUT_EMBED_HELPER) && defined(WABCUT_TEST_VISION_MEDIA)
TEST_CASE("Subject analysis throughput on a 1080p stream, DirectML against the CPU (WABCUT_TEST_SUBJECT_VIDEO)", "[ai][vision][model][perf]") {
    const auto dir = envPath("WABCUT_TEST_DETECT_DIR");
    const auto video = envPath("WABCUT_TEST_SUBJECT_VIDEO");
    if (!dir || !video) {
        SKIP("set WABCUT_TEST_DETECT_DIR and WABCUT_TEST_SUBJECT_VIDEO (a 1080p video)");
    }
    for (const std::string device : {"cpu", "auto"}) {
        INFO(device);
        HelperDetector detector(realDetector(*dir, device));
        MediaSubjectOptions options; // no cache: measured every time
        SubjectStats stats;
        auto result = analyzeFileSubjects(video->string(), -1, &detector, options, {}, &stats);
        REQUIRE(result);
        std::size_t withFaces = 0;
        for (const SubjectSample& s : result->samples) {
            withFaces += s.faces.empty() ? 0 : 1;
        }
        std::printf("[perf] subjects of a %.0f s 1080p stream on %s: %.1f s, %.2fx real time, %.1f samples/s (%zu samples, faces in %zu, "
                    "decoder %s)\n",
                    stats.analysedSeconds, stats.device.c_str(), stats.seconds, stats.analysedSeconds / stats.seconds,
                    static_cast<double>(result->samples.size()) / stats.seconds, result->samples.size(), withFaces, stats.backend.c_str());
        CHECK(result->samples.size() >= static_cast<std::size_t>(stats.analysedSeconds * 4.0));
        CHECK(withFaces * 2 >= result->samples.size());
    }
}

#if defined(WABCUT_CLI_PATH) && defined(WABCUT_FFMPEG) && defined(WABCUT_TEST_OUTPUT_DIR)
namespace {

int runTool(const std::filesystem::path& exe, std::vector<std::string> args) {
    ai::ChildProcessOptions o;
    o.executable = exe;
    o.arguments = std::move(args);
    auto child = ai::ChildProcess::spawn(o);
    REQUIRE(child);
    std::string line;
    while ((*child)->readLine(line, std::chrono::milliseconds(200)) != ai::ChildProcess::ReadStatus::Closed) {
    }
    return (*child)->wait(std::chrono::minutes(5)).value_or(-1);
}

} // namespace

TEST_CASE("A real clip reframed to 9:16 by the detectors, rendered (WABCUT_TEST_SUBJECT_VIDEO)", "[ai][vision][model][render]") {
    const auto dir = envPath("WABCUT_TEST_DETECT_DIR");
    const auto video = envPath("WABCUT_TEST_SUBJECT_VIDEO");
    if (!dir || !video) {
        SKIP("set WABCUT_TEST_DETECT_DIR and WABCUT_TEST_SUBJECT_VIDEO");
    }
    const std::string path = video->string();
    auto info = media::probe(path);
    REQUIRE(info);
    REQUIRE_FALSE(info->video.empty());
    MediaItem item;
    item.name = video->filename().string();
    item.locator.path = path;
    item.info = *info;
    ProjectState state;
    edit::SequenceSettings settings;
    settings.name = "Clip";
    settings.frameRate = info->video.front().frameRate;
    settings.width = info->video.front().width;
    settings.height = info->video.front().height;
    auto seq = edit::addSequence(state, settings);
    REQUIRE(seq);
    auto media = edit::addMedia(seq->state, item);
    REQUIRE(media);
    state = media->state;
    const Sequence& sequence = *state.findSequence(seq->id);
    edit::SourceClip source{media->id, TimeRange{Ticks{}, info->video.front().duration}, edit::defaultSourcePatches(sequence, *state.findMedia(media->id))};
    auto inserted = edit::insertEdit(state, seq->id, source, Ticks{});
    REQUIRE(inserted);
    state = inserted->state;

    HelperDetector detector(realDetector(*dir, "auto"));
    const ai::reframe::SubjectSource subjects = [&](const MediaItem&, int) {
        return analyzeFileSubjects(path, -1, &detector, MediaSubjectOptions{});
    };
    ai::reframe::ReframeOptions options; // 9:16, follow the main subject
    auto reframed = ai::reframe::reframeSequence(state, seq->id, options, subjects);
    REQUIRE(reframed);
    INFO(reframed->warnings.size());
    REQUIRE(reframed->clipsReframed == 1);
    std::printf("[render] reframed %s: %zu keyframes, guide %s\n", item.name.c_str(), reframed->keyframes,
                reframed->plans.begin()->second.guide.c_str());

    const std::filesystem::path out = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "reframe";
    std::filesystem::create_directories(out);
    const std::filesystem::path project = out / "reframe-render.wabcut";
    REQUIRE(io::saveProject(reframed->state, project));
    const std::filesystem::path movie = out / "reframed-9x16.mp4";
    std::filesystem::remove(movie);
    REQUIRE(runTool(WABCUT_CLI_PATH, {"render", project.string(), movie.string(), "--sequence", std::to_string(reframed->sequence.value),
                                      "--no-audio"}) == 0);
    REQUIRE(std::filesystem::exists(movie));
    for (const auto& [name, file, at] : {std::tuple{"reframed", movie, "6"}, std::tuple{"source", std::filesystem::path(path), "6"}}) {
        // BMP stills: this FFmpeg build has no PNG encoder.
        const std::filesystem::path still = out / (std::string(name) == "reframed" ? "reframed-9x16.bmp" : "source-16x9.bmp");
        std::filesystem::remove(still);
        REQUIRE(runTool(WABCUT_FFMPEG, {"-hide_banner", "-loglevel", "error", "-y", "-ss", at, "-i", file.string(), "-frames:v", "1",
                                        still.string()}) == 0);
        CHECK(std::filesystem::file_size(still) > 10000);
    }
}
#endif
#endif
