#pragma once

// The models of the AI video effects in wabcut-embed (EmbedProtocol.h, process_picture): ONNX Runtime with DirectML on the
// GPU and the CPU as fallback, out of the editor's process, so a model that crashes, runs out of memory or takes a driver
// reset with it ends one request, never the editor. Pictures go through raw files; the helper tiles them. After a crash
// the helper restarts on the CPU and the run goes on (up to maxRestarts times per picture); a cancel kills it.

#include <wabcut/ai/ModelStore.h>
#include <wabcut/ai/video/Backend.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace wabcut::ai::video {

struct HelperBackendOptions {
    std::filesystem::path executable;     ///< search::defaultEmbedExecutable() when empty
    std::vector<std::string> arguments;   ///< extra helper arguments (tests script a fake helper with them)
    std::filesystem::path modelRoot;      ///< the ModelStore root; ModelStore::defaultRoot() when empty
    /// Where a model's file is, or nullopt when it is not installed (tests point it at stand-ins); default: the ModelStore.
    std::function<std::optional<std::filesystem::path>(Model)> resolveModel;
    std::string device = "auto";          ///< "auto" (GPU, else CPU), "gpu", "cpu"
    int threads = 0;
    std::filesystem::path workDir;        ///< picture files; <temp>/wabcut-embed when empty
    std::filesystem::path log;            ///< the helper's stderr; empty = discarded
    std::chrono::milliseconds startTimeout{std::chrono::seconds(20)};
    std::chrono::milliseconds loadTimeout{std::chrono::seconds(180)};   ///< DirectML compiles a model at its first run
    std::chrono::milliseconds requestBase{std::chrono::seconds(120)};   ///< a picture's time, plus ...
    std::chrono::milliseconds requestPerMegapixel{std::chrono::seconds(45)}; ///< ... this per megapixel of output
    int maxRestarts = 3;                  ///< per picture
};

class HelperBackend final : public IVideoBackend {
public:
    explicit HelperBackend(HelperBackendOptions options);
    ~HelperBackend() override;

    std::expected<StepResult, Error> run(const Picture& in, const Step& step, std::stop_token stop) override;
    std::expected<StepResult, Error> interpolate(const Picture& a, const Picture& b, double timestep, std::stop_token stop) override;
    std::string device() const override;
    void release() override;

    /// Helper processes started so far.
    int starts() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace wabcut::ai::video
