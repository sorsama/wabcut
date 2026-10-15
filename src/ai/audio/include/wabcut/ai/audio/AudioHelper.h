#pragma once

// The audio models in the wabcut-embed helper (EmbedProtocol.h "enhance_audio" / "separate_audio"; A.4). Like smart
// search and the subject detectors it runs the models out of process: a crash, a timeout, a protocol violation or a
// cancel kills the helper and the call ends with a clean Error; the next call starts a new helper and loads the model
// again. A crash while a block is processed is retried once (a GPU driver reset should not fail an hour-long job);
// a timeout or a cancel is not. Blocks travel as raw float files in `workDir`, deleted after each call.

#include <wabcut/ai/search/EmbedHelper.h>
#include <wabcut/model/Error.h>

#include <chrono>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace wabcut::ai::audio {

struct AudioHelperOptions {
    std::filesystem::path executable;   ///< search::defaultEmbedExecutable() when empty
    std::vector<std::string> arguments; ///< extra helper arguments (tests script a fake helper with them)
    std::filesystem::path speechModel;  ///< DeepFilterNet3's directory (speechModelDirectory); empty = no enhance
    std::filesystem::path stemModel;    ///< HTDemucs .onnx; empty = no separation
    std::string device = "auto";        ///< "auto" (GPU, else CPU), "gpu", "cpu"
    int threads = 0;
    std::filesystem::path workDir;      ///< block files; <temp>/wabcut-audio when empty
    std::filesystem::path log;          ///< the helper's stderr; empty = discarded
    std::chrono::milliseconds startTimeout{std::chrono::seconds(20)};
    std::chrono::milliseconds loadTimeout{std::chrono::seconds(120)};
    std::chrono::milliseconds requestTimeout{std::chrono::seconds(600)};
    int crashRetries = 1;               ///< restarts per block after a crash
    bool lowPriority = true;            ///< the helper below normal priority: never competes with playback
};

class AudioHelper {
public:
    explicit AudioHelper(AudioHelperOptions options);
    ~AudioHelper(); ///< shuts the helper down
    AudioHelper(const AudioHelper&) = delete;
    AudioHelper& operator=(const AudioHelper&) = delete;

    /// Enhance Speech over one block of interleaved 48 kHz samples: the enhanced block, same layout and length.
    /// Errors: NotFound (no helper; no speech model configured or its files missing), DecoderError (crashed again,
    /// timed out, answered wrongly), VersionMismatch, Cancelled.
    std::expected<std::vector<float>, Error> enhance(std::span<const float> interleaved, int channels, std::stop_token stop);

    /// Stems of one stereo segment (interleaved, at `rate`, at most the model's segment): `stems` blocks of frames x 2
    /// interleaved, one after the other. Errors as enhance() (DecoderError when the model returns another number of
    /// stems).
    std::expected<std::vector<float>, Error> separate(std::span<const float> interleaved, int rate, int stems, std::stop_token stop);

    /// What the models run on ("DirectML", "CPU"; "" before the first call).
    std::string device() const;
    /// Helpers started so far (a crash restarts it).
    int starts() const;
    /// Time the helper spent on the requests, as it reports (milliseconds, in total).
    double inferenceMilliseconds() const;
    /// Stops the helper now (it frees the models' memory).
    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace wabcut::ai::audio
