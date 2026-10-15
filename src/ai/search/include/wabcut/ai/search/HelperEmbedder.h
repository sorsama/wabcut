#pragma once

// SigLIP 2 in the wabcut-embed helper (src/ai_local/embed, ONNX Runtime; EmbedProtocol.h). The helper starts on the
// first call and keeps its models loaded, so a search costs one text encoding (tens of milliseconds), not a model
// load. A crash, a timeout, a protocol violation or a cancel kills it; the call fails with a clean Error and the next
// call starts a new helper. Calls are serialised (one helper, one request at a time).

#include <wabcut/ai/ModelStore.h>
#include <wabcut/ai/search/EmbedHelper.h>
#include <wabcut/ai/search/Embedding.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace wabcut::ai::search {

struct HelperEmbedderOptions {
    std::filesystem::path executable;   ///< defaultEmbedExecutable() when empty
    std::vector<std::string> arguments; ///< extra helper arguments (tests script a fake helper with them)
    std::filesystem::path visionModel;
    std::filesystem::path textModel;
    std::filesystem::path tokenizer;
    std::string modelVersion;           ///< Embedder::modelVersion()
    int dimension = 768;
    std::string device = "auto";        ///< "auto" (GPU, else CPU), "gpu", "cpu"
    int threads = 0;
    int inputSize = 224;                ///< pictures are resized (resizeBilinear) to inputSize x inputSize
    int batchSize = 16;
    std::filesystem::path workDir;      ///< picture batch files; <temp>/wabcut-embed when empty
    std::filesystem::path log;          ///< the helper's stderr; empty = discarded
    std::chrono::milliseconds startTimeout{std::chrono::seconds(20)};
    std::chrono::milliseconds loadTimeout{std::chrono::seconds(180)};
    std::chrono::milliseconds requestTimeout{std::chrono::seconds(120)};
};

/// The built-in smart-search model: SigLIP 2 base (patch 16, 224 px), 8-bit, from the model manifest.
inline constexpr std::string_view kSiglipModelId = "siglip2-base-patch16-224";
/// Version of the vectors it makes (weights and preprocessing); changes whenever either does.
std::string siglipModelVersion();
/// The three manifest entries (vision encoder, text encoder, tokenizer).
std::vector<const ModelInfo*> siglipModelFiles();
/// Options for SigLIP 2 with the files of `store` (installed or not: the helper reports a missing file).
HelperEmbedderOptions siglipOptions(const ModelStore& store);

class HelperEmbedder : public Embedder {
public:
    explicit HelperEmbedder(HelperEmbedderOptions options);
    ~HelperEmbedder() override; ///< shuts the helper down

    HelperEmbedder(const HelperEmbedder&) = delete;
    HelperEmbedder& operator=(const HelperEmbedder&) = delete;

    std::expected<std::vector<Vector>, Error> embedImages(std::span<const Image> images, std::stop_token stop) override;
    std::expected<Vector, Error> embedText(std::string_view text, std::stop_token stop) override;
    std::string modelVersion() const override;
    int dimension() const override;
    std::string device() const override;

    /// Stops the helper now (it frees the models' memory); the next call starts it again.
    void shutdown();
    /// How many helpers were started (tests: a crash restarts it).
    int starts() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace wabcut::ai::search
