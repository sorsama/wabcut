#pragma once

// SAM 2.1 in wabcut-embed (AI-14; docs/ai.md "AI Object Mask"): the Segmenter the app and wabcut-cli use. The model
// runs out of process (EmbedHelper; ONNX Runtime, DirectML with the CPU as fallback), one object at a time: a prompt
// starts it, each track call segments the next picture from its memory. Pictures go through a raw RGB batch file and
// mattes come back through another (EmbedProtocol.h, segment_frame).
//
// Crashes: when the GPU provider takes the helper down (an access violation is no exception), the helper restarts on
// the CPU and the run goes on: the prompt is replayed, then the frame retried (the object's recent memory restarts from
// the prompt). A helper that keeps crashing ends the run after three restarts.

#include <wabcut/ai/ModelStore.h>
#include <wabcut/ai/segment/Propagation.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace wabcut::ai::segment {

struct HelperSegmenterOptions {
    std::filesystem::path executable;   ///< search::defaultEmbedExecutable() when empty
    std::vector<std::string> arguments; ///< extra helper arguments (tests script a fake helper with them)
    std::filesystem::path modelDirectory; ///< the sam2.1-tiny-video directory (segmentModelDirectory())
    std::string modelVersion;           ///< Segmenter::modelVersion()
    std::string device = "auto";        ///< "auto" (GPU, else CPU), "gpu", "cpu"
    int threads = 0;
    std::filesystem::path workDir;      ///< picture and matte files; <temp>/wabcut-embed when empty
    std::filesystem::path log;          ///< the helper's stderr; empty = discarded
    std::chrono::milliseconds startTimeout{std::chrono::seconds(20)};
    std::chrono::milliseconds loadTimeout{std::chrono::seconds(120)};
    std::chrono::milliseconds requestTimeout{std::chrono::seconds(60)};
    int maxRestarts = 3;                ///< per prompt
};

/// The model's files (ModelStore entries), the version of the mattes (the encoder's and decoder's digests and the
/// pre/post-processing), and the directory they install to.
std::vector<const ModelInfo*> segmentModelFiles();
std::string segmentModelVersion();
std::filesystem::path segmentModelDirectory(const ModelStore& store);
bool segmentModelInstalled(const ModelStore& store);
/// Options for the installed (or not yet installed) model of `store`.
HelperSegmenterOptions segmenterOptions(const ModelStore& store);

class HelperSegmenter final : public Segmenter {
public:
    explicit HelperSegmenter(HelperSegmenterOptions options);
    ~HelperSegmenter() override;

    int inputSize() const override { return 1024; }
    std::string modelVersion() const override;
    std::expected<Segmentation, Error> prompt(const search::Image& picture, const Prompt& prompt, int frame, int frames, int width,
                                               int height, std::stop_token stop) override;
    std::expected<Segmentation, Error> track(const search::Image& picture, int frame, int frames, int width, int height,
                                              std::stop_token stop) override;

    /// What the model runs on ("DirectML", "CPU"; "" before it loaded).
    std::string device() const;
    /// Helper processes started so far.
    int starts() const;
    /// Shuts the helper down (frees the model's memory); the next call starts it again.
    void release();

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace wabcut::ai::segment
