#pragma once

// Local caption translation (AI-12, docs/ai.md "Going global"): OPUS-MT models (Helsinki-NLP; Apache-2.0, two
// CC-BY-4.0) run by the wabcut-embed helper on ONNX Runtime (DirectML, else the CPU), optional downloads pinned in
// the model manifest (ModelStore.h, kind Translation). Nothing leaves the computer.
//
//   English -> Thai, Japanese, Vietnamese, Indonesian, Lao, Khmer, Burmese   opus-mt-en-mul (target token >>tha<< ...)
//   English -> Spanish, French, German, Chinese (Simplified / Traditional)   opus-mt-en-es / -fr / -de / -zh
//   Thai, Japanese, Korean, Chinese, Spanish, French, German -> English      opus-mt-<xx>-en
//
// en-mul covers many languages with one small model, so its Thai is rougher than a cloud model's: the dialog says
// so and suggests the cloud model where quality matters.

#include <wabcut/ai/Capability.h>
#include <wabcut/ai/ModelStore.h>
#include <wabcut/ai/search/EmbedHelper.h>

#include <chrono>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai::translate {

struct LocalPair {
    std::string_view source;      ///< normalized language codes (Languages.h)
    std::string_view target;
    std::string_view model;       ///< "opus-mt-en-mul"
    std::string_view targetToken; ///< ">>tha<<" for a multilingual model, else ""
};

std::span<const LocalPair> localPairs();
const LocalPair* findLocalPair(std::string_view source, std::string_view target);

/// The model's files in the manifest (encoder, decoder, tokenizer, config), or empty for an unknown model.
std::vector<const ModelInfo*> localModelFiles(std::string_view model);
/// Where the store keeps them: <store>/<model>/.
std::filesystem::path localModelDirectory(const ModelStore& store, std::string_view model);
bool isLocalModelInstalled(const ModelStore& store, std::string_view model);
/// Download size of the files not installed yet.
std::uint64_t localModelDownloadBytes(const ModelStore& store, std::string_view model);
/// Downloads what is missing (pinned, hash-checked). Errors as ModelStore::ensure.
std::expected<std::filesystem::path, Error> ensureLocalModel(ModelStore& store, std::string_view model, IDownloadTransport* transport,
                                                             const std::function<void(const DownloadProgress&)>& progress,
                                                             std::stop_token stop);

struct LocalTranslatorOptions {
    std::filesystem::path executable;   ///< search::defaultEmbedExecutable() when empty
    std::vector<std::string> arguments; ///< tests script a fake helper with them
    std::filesystem::path modelDirectory;
    std::string targetToken;
    /// "cpu" by default: DirectML (ONNX Runtime 1.24.4) crashes creating a session for these 8-bit dynamic-quantized
    /// graphs (measured 2026-10-12). "auto" / "gpu" try it and, after such a crash, restart the helper on the CPU.
    std::string device = "cpu";
    int threads = 0;
    std::filesystem::path log;
    std::chrono::milliseconds startTimeout{std::chrono::seconds(20)};
    std::chrono::milliseconds loadTimeout{std::chrono::seconds(120)};
    std::chrono::milliseconds requestTimeout{std::chrono::seconds(300)};
    std::size_t batch = 16;             ///< texts per helper request
};

/// ITranslator over the helper. A crash or a timeout ends the call with an error (the next starts a new helper).
class LocalTranslator final : public ITranslator {
public:
    explicit LocalTranslator(LocalTranslatorOptions options);
    ~LocalTranslator() override;

    std::expected<TranslationResult, AiError> translate(const TranslationRequest& request, std::stop_token stop) override;

    /// What it ran on ("DirectML", "CPU"; "" before the first call), tokens generated and helper time (ms) so far.
    const std::string& device() const { return m_device; }
    std::int64_t tokens() const { return m_tokens; }
    double milliseconds() const { return m_milliseconds; }

private:
    LocalTranslatorOptions m_options;
    std::unique_ptr<search::EmbedHelper> m_helper;
    int m_loadedSession = 0;
    std::string m_device;
    bool m_cpuOnly = false; ///< the GPU crashed the helper once: the CPU from then on
    std::int64_t m_tokens = 0;
    double m_milliseconds = 0.0;
};

} // namespace wabcut::ai::translate
