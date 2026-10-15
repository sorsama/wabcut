#pragma once

// Local model weights: a built-in manifest (id, file, URL, size, SHA-256, license) and a store in the user cache
// that downloads a model on first use, resumes interrupted downloads and verifies every byte (docs/ai.md,
// "Provider contract"). The network goes through an injected IDownloadTransport; tests use a fake one and never
// download.

#include <wabcut/model/Error.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>

namespace wabcut::ai {

/// Embedding: the files of the smart-search model (SigLIP 2's image and text encoders and its tokenizer).
/// Llm: GGUF weights for the local assistant (llama-server, AI-10). Runtime: an official llama.cpp release archive
/// (the llama-server program and its backends), extracted by LocalRuntimeStore (wabcut/ai/llm/LocalModels.h).
/// Audio: the local audio models (A.4): DeepFilterNet3 for Enhance Speech (an archive of three ONNX graphs, extracted
/// by wabcut/ai/audio/AudioModels.h) and HTDemucs for stem separation.
/// Segmentation (AI-14): SAM 2.1 Hiera-Tiny's video tracker, five ONNX graphs and its constants in one directory.
enum class ModelKind { Whisper, Vad, Embedding, Detection, Llm, Runtime, Audio, Translation, Segmentation };

struct ModelInfo {
    std::string id;          ///< "base", "large-v3-turbo-q5_0", "silero-v6.2.0", ...
    ModelKind kind = ModelKind::Whisper;
    std::string fileName;    ///< name in the store
    std::string url;         ///< HTTPS, pinned to a repository revision
    std::uint64_t size = 0;  ///< bytes
    std::string sha256;      ///< 64 lowercase hex digits
    std::string license;     ///< SPDX id of the weights
    std::string licenseCard; ///< shown before the first download
    std::string description;
};

/// The built-in manifest: whisper tiny, base and large-v3-turbo (q5_0), the Silero VAD model, SigLIP 2 base
/// (vision encoder, text encoder, tokenizer) for smart search, the subject detectors, DeepFilterNet3 and HTDemucs, and the
/// OPUS-MT caption translation models (AI-12; four files each, in a directory of their own: "opus-mt-<pair>/...").
std::span<const ModelInfo> builtinModels();

/// Looks up a model by id or alias ("large-v3-turbo" -> "large-v3-turbo-q5_0", "silero" -> current VAD model).
const ModelInfo* findModel(std::string_view idOrAlias);

/// The licence allow-list for downloadable weights and runtimes (docs/ai.md: no NC, no AGPL, no community licences):
/// MIT, Apache-2.0, BSD-2-Clause, BSD-3-Clause, ISC, Zlib, and CC-BY-4.0 (attribution only: the model's card, shown
/// before the download and in THIRD-PARTY-NOTICES, carries it; AI-12). Every built-in entry passes it (a test checks), except the
/// optional NVIDIA CUDA runtime, which needs the user's explicit opt-in (requiresLicenseOptIn).
bool isAllowedModelLicense(std::string_view spdx);
/// A redistributable under its vendor's own terms ("LicenseRef-..."): downloaded only after the user accepts its card.
bool requiresLicenseOptIn(const ModelInfo& model);

/// HTTP GET with an optional byte-range start. Implementations: WinHTTP (ai_local), Qt (ai_net), test fakes.
class IDownloadTransport {
public:
    struct Response {
        int status = 0;                           ///< HTTP status after redirects
        std::optional<std::uint64_t> totalSize;   ///< size of the whole resource, when the server says
    };
    virtual ~IDownloadTransport() = default;
    /// Requests `url` from byte `offset` on (a Range request when offset > 0). `onResponse` is called once, with
    /// the final status (after redirects), before any body byte; then the body streams to `onData`. Returning
    /// false from either aborts. A server that ignores the range answers 200 and sends the whole body.
    /// Errors: IoError (network), Cancelled (stop requested or a callback returned false).
    virtual std::expected<Response, Error> get(const std::string& url, std::uint64_t offset,
                                               const std::function<bool(const Response&)>& onResponse,
                                               const std::function<bool(std::span<const std::byte>)>& onData,
                                               std::stop_token stop) = 0;
};

struct DownloadProgress {
    std::uint64_t received = 0; ///< bytes on disk, including a resumed part
    std::uint64_t total = 0;
};

class ModelStore {
public:
    /// `root` is created on demand. defaultRoot(): <user cache>/models.
    explicit ModelStore(std::filesystem::path root);
    static std::filesystem::path defaultRoot();

    const std::filesystem::path& root() const { return m_root; }
    std::filesystem::path pathFor(const ModelInfo& model) const;

    /// The model file is complete and was verified when it was stored (size and digest marker match).
    bool isInstalled(const ModelInfo& model) const;

    /// The local file of `model`, downloading it first if needed (resuming a previous partial download). With
    /// no transport a missing model is a NotFound error ("not downloaded"). Errors: NotFound, IoError,
    /// Cancelled, InvalidArgument (size or SHA-256 mismatch: the partial file is deleted and the next call
    /// starts over).
    std::expected<std::filesystem::path, Error> ensure(const ModelInfo& model, IDownloadTransport* transport,
                                                       const std::function<void(const DownloadProgress&)>& progress = {},
                                                       std::stop_token stop = {});

    /// Re-hashes an installed file (slow for large models). InvalidArgument on mismatch.
    std::expected<void, Error> verify(const ModelInfo& model) const;
    /// Deletes the model and any partial download.
    std::expected<void, Error> remove(const ModelInfo& model);

private:
    std::filesystem::path m_root;
};

/// The per-user cache directory for Wabcut: %LOCALAPPDATA%\Wabcut\cache on Windows, ~/Library/Caches/Wabcut on
/// macOS, $XDG_CACHE_HOME/wabcut (or ~/.cache/wabcut) elsewhere. The WABCUT_CACHE_DIR environment variable
/// overrides it.
std::filesystem::path userCacheDir();

} // namespace wabcut::ai
