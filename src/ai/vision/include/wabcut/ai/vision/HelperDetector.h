#pragma once

// YuNet faces and YOLOX people in the wabcut-embed helper (src/ai_local/embed; EmbedProtocol.h "detect"), the
// SubjectDetector of auto reframe and the shorts finder (AI-9). It shares EmbedHelper with smart search, so it has
// the same crash handling: a crash, a timeout, a protocol violation or a cancel kills the helper, the call fails with
// a clean Error and the next call starts a new one (and loads the detectors again).

#include <wabcut/ai/ModelStore.h>
#include <wabcut/ai/search/EmbedHelper.h>
#include <wabcut/ai/vision/Subjects.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace wabcut::ai::vision {

struct HelperDetectorOptions {
    std::filesystem::path executable;   ///< search::defaultEmbedExecutable() when empty
    std::vector<std::string> arguments; ///< extra helper arguments (tests script a fake helper with them)
    std::filesystem::path faceModel;    ///< YuNet .onnx; empty = no faces
    std::filesystem::path objectModel;  ///< YOLOX .onnx; empty = no people
    std::string modelVersion;           ///< SubjectDetector::modelVersion()
    std::string device = "auto";        ///< "auto" (GPU, else CPU), "gpu", "cpu"
    int threads = 0;
    int batchSize = 8;                  ///< pictures per request
    float faceThreshold = 0.6f;
    float objectThreshold = 0.35f;
    bool allClasses = false;            ///< also every COCO class, into FrameDetections::objects (AI-14)
    std::filesystem::path workDir;      ///< picture batch files; <temp>/wabcut-embed when empty
    std::filesystem::path log;          ///< the helper's stderr; empty = discarded
    std::chrono::milliseconds startTimeout{std::chrono::seconds(20)};
    std::chrono::milliseconds loadTimeout{std::chrono::seconds(60)};
    std::chrono::milliseconds requestTimeout{std::chrono::seconds(120)};
};

/// The two manifest entries (YuNet 2023mar, YOLOX-Tiny).
std::vector<const ModelInfo*> subjectModelFiles();
/// Version of the detections (weights' digests and the decoding); cache keys change whenever it does.
std::string subjectModelVersion();
/// Options for the built-in detectors with the files of `store` (installed or not: the helper reports a missing file).
HelperDetectorOptions subjectDetectorOptions(const ModelStore& store);
/// Whether both detector files are installed in `store`.
bool subjectModelsInstalled(const ModelStore& store);

class HelperDetector : public SubjectDetector {
public:
    explicit HelperDetector(HelperDetectorOptions options);
    ~HelperDetector() override; ///< shuts the helper down

    HelperDetector(const HelperDetector&) = delete;
    HelperDetector& operator=(const HelperDetector&) = delete;

    /// Errors: NotFound (no helper or model file), DecoderError (a crash, a timeout, a bad answer), VersionMismatch,
    /// InvalidArgument (an empty picture), Cancelled.
    std::expected<std::vector<FrameDetections>, Error> detect(std::span<const Image> frames, std::stop_token stop) override;
    std::string modelVersion() const override;

    /// What the detectors run on ("DirectML", "CPU"; "" before the first call).
    std::string device() const;
    /// Stops the helper now (it frees the models' memory); the next call starts it again.
    void shutdown();
    /// How many helpers were started (tests: a crash restarts it).
    int starts() const;
    /// Time the helper spent detecting, in total (milliseconds, as it reports).
    double inferenceMilliseconds() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace wabcut::ai::vision
