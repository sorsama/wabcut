#include <wabcut/ai/ModelStore.h>

#include "TranslationManifest.h"

#include <wabcut/ai/Sha256.h>
#include <wabcut/model/MediaLocator.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

namespace wabcut::ai {

namespace {

// Whisper weights (MIT, OpenAI) converted to ggml by the whisper.cpp project; Silero VAD (MIT) converted by
// ggml-org. URLs are pinned to repository revisions, sizes and digests come from the repositories' LFS metadata.
constexpr std::string_view kWhisperRepo =
    "https://huggingface.co/ggerganov/whisper.cpp/resolve/5359861c739e955e79d9a303bcbc70fb988958b1/";
constexpr std::string_view kVadRepo =
    "https://huggingface.co/ggml-org/whisper-vad/resolve/9ffd54a1e1ee413ddf265af9913beaf518d1639b/";

constexpr std::string_view kWhisperCard =
    "OpenAI Whisper model weights, MIT License (Copyright (c) 2022 OpenAI), converted to the ggml format by the "
    "whisper.cpp project (MIT). Runs entirely on this computer; no audio leaves it. "
    "https://github.com/openai/whisper/blob/main/LICENSE";
constexpr std::string_view kVadCard =
    "Silero VAD v6.2.0, MIT License (Copyright (c) 2020-present Silero Team), converted to the ggml format by "
    "ggml-org. Voice activity detection; runs on this computer. https://github.com/snakers4/silero-vad";
/// SigLIP 2 base, patch 16, 224 px (Google, Apache-2.0), exported to ONNX and quantized to 8 bits by onnx-community.
constexpr std::string_view kSiglipRepo =
    "https://huggingface.co/onnx-community/siglip2-base-patch16-224-ONNX/resolve/ba1f3b0843f24bc5417d38e19c37b287d719b2f4/";
constexpr std::string_view kSiglipCard =
    "SigLIP 2 (google/siglip2-base-patch16-224, Google), Apache License 2.0, with its Gemma "
    "tokenizer vocabulary; ONNX export and 8-bit quantization by onnx-community. Smart search runs it entirely on this "
    "computer: no picture or search text leaves it. https://huggingface.co/google/siglip2-base-patch16-224";

/// Subject detection for auto reframe (AI-9): YuNet faces (OpenCV Zoo, MIT) and YOLOX-Tiny people (Megvii, Apache-2.0),
/// both ONNX as their authors published them.
constexpr std::string_view kYuNetUrl = "https://huggingface.co/opencv/face_detection_yunet/resolve/"
                                       "3cc26e7f1014a5ee5d74a42acee58bafc9d0a310/face_detection_yunet_2023mar.onnx";
constexpr std::string_view kYuNetCard =
    "YuNet face detector (face_detection_yunet_2023mar), MIT License (Copyright (c) 2020 Shiqi Yu), from the OpenCV "
    "Model Zoo. Auto reframe runs it on this computer: no picture leaves it. "
    "https://github.com/opencv/opencv_zoo/tree/main/models/face_detection_yunet";
constexpr std::string_view kYoloxUrl = "https://github.com/Megvii-BaseDetection/YOLOX/releases/download/0.1.1rc0/yolox_tiny.onnx";
constexpr std::string_view kYoloxCard =
    "YOLOX-Tiny object detector (Megvii, release 0.1.1rc0), Apache License 2.0 (Copyright (c) 2021-2022 Megvii Inc.), "
    "trained on COCO. Auto reframe uses its person class, on this computer: no picture leaves it. "
    "https://github.com/Megvii-BaseDetection/YOLOX";

/// Local audio models (A.4). DeepFilterNet3 (Hendrik Schroeter; code and weights dual MIT / Apache-2.0, used under
/// MIT): the ONNX archive of the repository at the commit that published the final model (2023-05-23); its three
/// graphs are extracted and verified one by one (wabcut/ai/audio/AudioModels.h). HTDemucs (Meta; MIT, weights
/// included, as the Demucs repository releases them): the single-file ONNX export with float16-stored weights by
/// StemSplit (MIT; export pipeline github.com/StemSplit/demucs-onnx), pinned to its Hugging Face revision.
constexpr std::string_view kDeepFilterUrl =
    "https://raw.githubusercontent.com/Rikorose/DeepFilterNet/84d57ec2c08fe08e68a13fb32a58cd7092060a0f/models/"
    "DeepFilterNet3_onnx.tar.gz";
constexpr std::string_view kDeepFilterCard =
    "DeepFilterNet3 speech enhancement (Hendrik Schroeter, github.com/Rikorose/DeepFilterNet), code and weights "
    "licensed under either of the MIT License or the Apache License 2.0; Wabcut uses them under the MIT License "
    "(Copyright (c) 2021 Hendrik Schröter). Enhance Speech runs it on this computer: no audio leaves it. "
    "https://github.com/Rikorose/DeepFilterNet/blob/main/LICENSE";
constexpr std::string_view kHtDemucsUrl =
    "https://huggingface.co/StemSplitio/htdemucs-onnx/resolve/d54ed9eb60e258ea82131c6ee14578628816456a/"
    "htdemucs_fp16weights.onnx";
constexpr std::string_view kHtDemucsCard =
    "HTDemucs music source separation (Simon Rouard, Francisco Massa, Alexandre Defossez; Meta), MIT License "
    "(Copyright (c) Meta Platforms, Inc. and affiliates), weights as released with Demucs v4; ONNX export by StemSplit "
    "(MIT License, Copyright (c) 2026 StemSplit). Separates vocals, drums, bass and other on this computer: no audio "
    "leaves it. https://github.com/facebookresearch/demucs";

/// AI Object Mask (AI-14): SAM 2.1 Hiera-Tiny (Meta, Apache-2.0, code and weights) as its full video tracker - image
/// encoder, mask decoder, memory encoder, memory attention and the object-pointer position encoding - exported from
/// transformers' Sam2VideoModel to fixed-shape fp32 ONNX by square-zero-labs (Apache-2.0), pinned to the revision
/// below; sizes and SHA-256 from the repository's LFS metadata (checked 2026-10-12; constants.json hashed on download).
constexpr std::string_view kSam2Repo = "square-zero-labs/sam2.1-tiny-video-onnx";
constexpr std::string_view kSam2Revision = "3b2984dd865f6e9d2cc6aed0be6a5a5c2eb352ce";
constexpr std::string_view kSam2Card =
    "SAM 2.1 Hiera-Tiny (Segment Anything Model 2.1, facebook/sam2.1-hiera-tiny; Nikhila Ravi et al., Meta FAIR), "
    "Apache License 2.0 (Copyright (c) Meta Platforms, Inc. and affiliates), code and model weights. ONNX export of its "
    "video tracker (memory encoder, memory attention, object pointers) from Hugging Face transformers' Sam2VideoModel by "
    "square-zero-labs, Apache License 2.0, revision 3b2984dd865f6e9d2cc6aed0be6a5a5c2eb352ce. The AI Object Mask runs "
    "it on this computer: no picture leaves it. https://github.com/facebookresearch/sam2/blob/main/LICENSE";

/// Local assistant weights (AI-10): GGUF files pinned to repository revisions; sizes and SHA-256 are the repositories'
/// LFS metadata (checked 2026-10-11). Only Apache-2.0 / MIT models (the allow-list); the cards quote each licence.
std::string hfUrl(std::string_view repo, std::string_view revision, std::string_view file) {
    return "https://huggingface.co/" + std::string(repo) + "/resolve/" + std::string(revision) + "/" + std::string(file);
}

ModelInfo llm(std::string id, std::string_view repo, std::string_view revision, std::string file, std::uint64_t size,
              std::string sha, std::string license, std::string card, std::string description) {
    std::string url = hfUrl(repo, revision, file);
    return ModelInfo{std::move(id), ModelKind::Llm, std::move(file), std::move(url), size, std::move(sha),
                     std::move(license), std::move(card), std::move(description)};
}

/// llama.cpp's official release archives (MIT), build b11429 (2026-10-05); sizes and SHA-256 from the
/// GitHub release API's asset digests. The CUDA runtime archives hold NVIDIA's redistributable cudart / cuBLAS DLLs
/// (NVIDIA CUDA EULA, Attachment A): downloaded only when the user picks the CUDA engine.
constexpr std::string_view kLlamaTag = "b11429";
constexpr std::string_view kLlamaCard =
    "llama.cpp llama-server (ggml-org, build b11429), MIT License (Copyright (c) 2023-2026 The ggml "
    "authors), as the project publishes it on GitHub. It runs the local assistant's model on this computer, on "
    "127.0.0.1 only. https://github.com/ggml-org/llama.cpp/blob/b11429/LICENSE";
constexpr std::string_view kCudaCard =
    "NVIDIA CUDA runtime and cuBLAS DLLs (cudart, cublas, cublasLt) as llama.cpp publishes them, under the NVIDIA CUDA "
    "Toolkit EULA (redistributable files, Attachment A), used only by the local assistant's llama-server. Needs a "
    "recent NVIDIA driver. https://docs.nvidia.com/cuda/eula/index.html";

ModelInfo runtime(std::string id, std::string file, std::uint64_t size, std::string sha, std::string description,
                  bool cuda = false) {
    std::string url = "https://github.com/ggml-org/llama.cpp/releases/download/" + std::string(kLlamaTag) + "/" + file;
    return ModelInfo{std::move(id), ModelKind::Runtime, std::move(file), std::move(url), size, std::move(sha),
                     cuda ? "LicenseRef-NVIDIA-CUDA-EULA" : "MIT", std::string(cuda ? kCudaCard : kLlamaCard),
                     std::move(description)};
}

std::vector<ModelInfo> llmManifest() {
    const std::string qwen = "Apache License 2.0 (Qwen team, Alibaba Cloud). Runs on this computer through llama-server: "
                             "nothing you ask the assistant leaves it. ";
    return {
        // The small tier (AI-15): Qwen's own GGUF repository has only Q8_0 (1.8 GB), so Unsloth's Q4_K_M quantization of
        // the same weights (base Qwen/Qwen3-1.7B, revision 70d244cc86ccca08cf5af4e1e306ecf908b1ad5e).
        llm("qwen3-1.7b", "unsloth/Qwen3-1.7B-GGUF", "d7f544eead698dbd1f15126ef60b45a1e1933222", "Qwen3-1.7B-Q4_K_M.gguf",
            1107409472, "b139949c5bd74937ad8ed8c8cf3d9ffb1e99c866c823204dc42c0d91fa181897", "Apache-2.0",
            "Qwen3-1.7B, " + qwen + "4-bit GGUF by Unsloth (license: apache-2.0). https://huggingface.co/Qwen/Qwen3-1.7B",
            "Qwen3 1.7B, 4-bit (1.1 GB): the small tier; runs in about 3 GB, on the GPU or the CPU"),
        llm("qwen3-4b-instruct-2507", "unsloth/Qwen3-4B-Instruct-2507-GGUF", "a06e946bb6b655725eafa393f4a9745d460374c9",
            "Qwen3-4B-Instruct-2507-Q4_K_M.gguf", 2497281120,
            "3605803b982cb64aead44f6c1b2ae36e3acdb41d8e46c8a94c6533bc4c67e597", "Apache-2.0",
            "Qwen3-4B-Instruct-2507, " + qwen + "4-bit GGUF by Unsloth (license: apache-2.0). "
            "https://huggingface.co/Qwen/Qwen3-4B-Instruct-2507",
            "Qwen3 4B Instruct 2507, 4-bit (2.5 GB): the default local assistant; memory fit checked before launch"),
        llm("qwen3-8b", "Qwen/Qwen3-8B-GGUF", "7c41481f57cb95916b40956ab2f0b139b296d974", "Qwen3-8B-Q4_K_M.gguf",
            5027783488, "d98cdcbd03e17ce47681435b5150e34c1417f50b5c0019dd560e4882c5745785", "Apache-2.0",
            "Qwen3-8B, " + qwen + "Official 4-bit GGUF. https://huggingface.co/Qwen/Qwen3-8B-GGUF",
            "Qwen3 8B, 4-bit (5.0 GB): memory fit checked before launch"),
        llm("phi-4-mini-instruct", "unsloth/Phi-4-mini-instruct-GGUF", "78eb92a46fc37e6b524df991ed9aca9bc6aa7b80",
            "Phi-4-mini-instruct-Q4_K_M.gguf", 2491874272,
            "88c00229914083cd112853aab84ed51b87bdf6b9ce42f532d8c85c7c63b1730a", "MIT",
            "Phi-4-mini-instruct (Microsoft), MIT License. 4-bit GGUF by Unsloth (license: mit). Tool calling is not enabled in this catalog "
            "pending template verification; it answers and ranks but cannot edit. "
            "https://huggingface.co/microsoft/Phi-4-mini-instruct",
            "Phi-4 mini, 4-bit (2.5 GB): chat and ranking only (no tool calls)"),
        runtime("llama-server-win-cpu-x64", "llama-b11429-bin-win-cpu-x64.zip", 19398918,
                "1283323272b04cd07905816a597a0da810918102de958f4ff6f7bbaa70ed2efe", "llama-server for Windows, CPU (19 MB)"),
        runtime("llama-server-win-vulkan-x64", "llama-b11429-bin-win-vulkan-x64.zip", 33337769,
                "1bfe78ad9168b79fa02bf67f6af9f5e17a966d824d77238517f7bef12ac73b36",
                "llama-server for Windows, Vulkan GPU (33 MB): NVIDIA, AMD and Intel GPUs"),
        runtime("llama-server-win-cuda-12.4-x64", "llama-b11429-bin-win-cuda-12.4-x64.zip", 264478423,
                "dd6df685c1024e6aa55ca55ad35038692d5e284d4dc895e7a646c3245d3d14ff",
                "llama-server for Windows, CUDA 12.4 (264 MB): NVIDIA with CUDA 12.4 driver support"),
        runtime("cudart-win-cuda-12.4-x64", "cudart-llama-bin-win-cuda-12.4-x64.zip", 391443627,
                "8c79a9b226de4b3cacfd1f83d24f962d0773be79f1e7b75c6af4ded7e32ae1d6",
                "NVIDIA CUDA 12.4 runtime for llama-server (391 MB)", true),

    };
}

std::vector<ModelInfo> makeManifest() {
    auto whisper = [](std::string id, std::string file, std::uint64_t size, std::string sha, std::string description) {
        return ModelInfo{std::move(id), ModelKind::Whisper, file, std::string(kWhisperRepo) + file, size,
                         std::move(sha), "MIT", std::string(kWhisperCard), std::move(description)};
    };
    auto sam2 = [](std::string id, std::string file, std::string_view repoPath, std::uint64_t size, std::string sha,
                   std::string description) {
        return ModelInfo{std::move(id), ModelKind::Segmentation, "sam2.1-tiny-video/" + file, hfUrl(kSam2Repo, kSam2Revision, repoPath),
                         size, std::move(sha), "Apache-2.0", std::string(kSam2Card), std::move(description)};
    };
    auto siglip = [](std::string id, std::string file, std::string_view repoPath, std::uint64_t size, std::string sha,
                     std::string description) {
        return ModelInfo{std::move(id), ModelKind::Embedding, std::move(file), std::string(kSiglipRepo) + std::string(repoPath),
                         size, std::move(sha), "Apache-2.0", std::string(kSiglipCard), std::move(description)};
    };
    return {
        whisper("tiny", "ggml-tiny.bin", 77691713, "be07e048e1e599ad46341c8d2a135645097a538221678b7acdd1b1919c6e1b21",
                "Whisper tiny, multilingual (78 MB): fastest, lowest accuracy; for tests and very slow machines"),
        whisper("base", "ggml-base.bin", 147951465, "60ed5bc3dd14eea856493d334349b405782ddcaf0028d4b5df4088345fba2efe",
                "Whisper base, multilingual (148 MB): the CPU-friendly preview tier"),
        whisper("large-v3-turbo-q5_0", "ggml-large-v3-turbo-q5_0.bin", 574041195,
                "394221709cd5ad1f40c46e6031ca61bce88931e6e088c188294c6d5a55ffa7e2",
                "Whisper large-v3-turbo, 5-bit (574 MB): near large-v3 quality, the default with a GPU"),
        ModelInfo{"silero-v6.2.0", ModelKind::Vad, "ggml-silero-v6.2.0.bin",
                  std::string(kVadRepo) + "ggml-silero-v6.2.0.bin", 885098,
                  "2aa269b785eeb53a82983a20501ddf7c1d9c48e33ab63a41391ac6c9f7fb6987", "MIT", std::string(kVadCard),
                  "Silero VAD v6.2.0 (0.9 MB): finds speech so silence is skipped"},
        siglip("siglip2-base-vision", "siglip2-base-patch16-224-vision-uint8.onnx", "onnx/vision_model_quantized.onnx",
               94553333, "5f2b401c1a4fc095702a5d45348e17ad46c4f87064085365b43c6e8eaa5c0070",
               "SigLIP 2 base image encoder, 8-bit (95 MB): what the footage shows, for smart search"),
        siglip("siglip2-base-text", "siglip2-base-patch16-224-text-int8.onnx", "onnx/text_model_quantized.onnx",
               283438275, "3a0603d3a00c05a80a6ded4743c16aaac7b1e62cdcc7e362e7ce418659b96400",
               "SigLIP 2 base text encoder, 8-bit (283 MB): turns a search into the same space as the footage"),
        siglip("siglip2-base-tokenizer", "siglip2-base-patch16-224-tokenizer.json", "tokenizer.json", 34363039,
               "cb9140fae3ac5122c972d37adf83e1248471a38147ad76f8215c8872c6fd8322",
               "SigLIP 2 tokenizer (Gemma vocabulary, 34 MB)"),
        ModelInfo{"yunet-2023mar", ModelKind::Detection, "face_detection_yunet_2023mar.onnx", std::string(kYuNetUrl), 232589,
                  "8f2383e4dd3cfbb4553ea8718107fc0423210dc964f9f4280604804ed2552fa4", "MIT", std::string(kYuNetCard),
                  "YuNet face detector (0.2 MB): where the faces are, for auto reframe and shorts"},
        ModelInfo{"yolox-tiny", ModelKind::Detection, "yolox_tiny.onnx", std::string(kYoloxUrl), 20219662,
                  "427cc366d34e27ff7a03e2899b5e3671425c262ea2291f88bb942bc1cc70b0f7", "Apache-2.0", std::string(kYoloxCard),
                  "YOLOX-Tiny detector (20 MB): where the people are, for auto reframe"},
        ModelInfo{"deepfilternet3", ModelKind::Audio, "DeepFilterNet3_onnx-84d57ec.tar.gz", std::string(kDeepFilterUrl), 7983136,
                  "c94d91f70911001c946e0fabb4aa9adc37045f45a03b56008cb0c8244cb63616", "MIT", std::string(kDeepFilterCard),
                  "DeepFilterNet3 (8 MB): Enhance Speech, noise and room removed from dialogue"},
        ModelInfo{"htdemucs", ModelKind::Audio, "htdemucs_fp16weights-d54ed9e.onnx", std::string(kHtDemucsUrl), 165612636,
                  "d05c269d0178d2a72ad484b10b11dd370193fc923201c3b27a99f848745db70a", "MIT", std::string(kHtDemucsCard),
                  "HTDemucs (166 MB): separates music into vocals, drums, bass and other"},
        sam2("sam2.1-tiny-video-encoder", "vision_encoder.onnx", "onnx/vision_encoder.onnx", 134335567,
             "aa7a8542942f042e235a993a1ab0ccf5f049918500577802a7f10ec1b39bb873",
             "SAM 2.1 tiny image encoder (134 MB): the picture's features, for the AI Object Mask"),
        sam2("sam2.1-tiny-video-decoder", "mask_decoder.onnx", "onnx/mask_decoder.onnx", 17794355,
             "0461896de3db00936fe1643506f129d71cb6d5d2ae15754811756b7ea1b070c6",
             "SAM 2.1 tiny prompt and mask decoder (18 MB): the object's matte from clicks or a box"),
        sam2("sam2.1-tiny-video-memory-attention", "memory_attention.onnx", "onnx/memory_attention.onnx", 32259165,
             "791e648ce8f5ef91ad00ba06e83066ff261ae5a645f0b042b4f46a89fd054baf",
             "SAM 2.1 tiny memory attention (32 MB): follows the object from frame to frame"),
        sam2("sam2.1-tiny-video-memory-encoder", "memory_encoder.onnx", "onnx/memory_encoder.onnx", 5616496,
             "580d246c109de88838f600ba7c1c0d03d1fe267f7641b06f8c88c5f0dc5834cd",
             "SAM 2.1 tiny memory encoder (6 MB): remembers the object's mattes"),
        sam2("sam2.1-tiny-video-pointer-pe", "pointer_tpos.onnx", "onnx/pointer_tpos.onnx", 67289,
             "7e71df1d75dba09bc18dd4ae745c2a2cceebdd14d84eadea2fe65d0205f101fc",
             "SAM 2.1 tiny object-pointer positions (0.1 MB)"),
        sam2("sam2.1-tiny-video-constants", "constants.json", "constants.json", 9922,
             "172edc70e892aab0aef6caedf97b8e4091fbf6c5d26dac5fd9ea69d093d927d3",
             "SAM 2.1 tiny constants (normalisation, memory positions)"),
    };
}

const std::vector<ModelInfo>& manifest() {
    static const std::vector<ModelInfo> models = [] {
        std::vector<ModelInfo> all = makeManifest();
        for (ModelInfo& m : llmManifest()) {
            all.push_back(std::move(m));
        }
        for (ModelInfo& m : translationManifest()) {
            all.push_back(std::move(m));
        }
        return all;
    }();
    return models;
}

std::filesystem::path markerFor(const std::filesystem::path& file) {
    std::filesystem::path p = file;
    p += ".verified";
    return p;
}

std::filesystem::path partFor(const std::filesystem::path& file) {
    std::filesystem::path p = file;
    p += ".part";
    return p;
}

std::string readSmallFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::string s;
    std::getline(in, s);
    return s;
}

/// Feeds a whole file to `h`; false on a read error.
bool hashFile(const std::filesystem::path& p, Sha256& h, std::uint64_t& bytes) {
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        return false;
    }
    std::vector<char> buffer(1 << 20);
    bytes = 0;
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto got = static_cast<std::size_t>(in.gcount());
        h.update(std::string_view(buffer.data(), got));
        bytes += got;
    }
    return !in.bad();
}

std::string envOr(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) {
        return {};
    }
    std::string s(value);
    std::free(value);
    return s;
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string{};
#endif
}

std::filesystem::path fromUtf8(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }

} // namespace

std::span<const ModelInfo> builtinModels() { return manifest(); }

bool isAllowedModelLicense(std::string_view spdx) {
    static constexpr std::array<std::string_view, 7> kAllowed{"MIT", "Apache-2.0", "BSD-2-Clause", "BSD-3-Clause",
                                                             "ISC", "Zlib", "CC-BY-4.0"};
    return std::find(kAllowed.begin(), kAllowed.end(), spdx) != kAllowed.end();
}

bool requiresLicenseOptIn(const ModelInfo& model) { return model.license.starts_with("LicenseRef-"); }

const ModelInfo* findModel(std::string_view idOrAlias) {
    std::string_view id = idOrAlias;
    if (id == "large-v3-turbo" || id == "turbo") {
        id = "large-v3-turbo-q5_0";
    } else if (id == "silero" || id == "vad") {
        id = "silero-v6.2.0";
    }
    for (const ModelInfo& m : manifest()) {
        if (m.id == id) {
            return &m;
        }
    }
    return nullptr;
}

std::filesystem::path userCacheDir() {
    if (const std::string overrideDir = envOr("WABCUT_CACHE_DIR"); !overrideDir.empty()) {
        return fromUtf8(overrideDir);
    }
#if defined(_WIN32)
    if (const std::string local = envOr("LOCALAPPDATA"); !local.empty()) {
        return fromUtf8(local) / "Wabcut" / "cache";
    }
    return std::filesystem::temp_directory_path() / "Wabcut" / "cache";
#elif defined(__APPLE__)
    const std::string home = envOr("HOME");
    return (home.empty() ? std::filesystem::temp_directory_path() : std::filesystem::path(home)) / "Library" /
           "Caches" / "Wabcut";
#else
    if (const std::string xdg = envOr("XDG_CACHE_HOME"); !xdg.empty()) {
        return std::filesystem::path(xdg) / "wabcut";
    }
    const std::string home = envOr("HOME");
    return (home.empty() ? std::filesystem::temp_directory_path() : std::filesystem::path(home)) / ".cache" / "wabcut";
#endif
}

ModelStore::ModelStore(std::filesystem::path root) : m_root(std::move(root)) {}

std::filesystem::path ModelStore::defaultRoot() { return userCacheDir() / "models"; }

std::filesystem::path ModelStore::pathFor(const ModelInfo& model) const { return m_root / model.fileName; }

bool ModelStore::isInstalled(const ModelInfo& model) const {
    const std::filesystem::path file = pathFor(model);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec) || std::filesystem::file_size(file, ec) != model.size || ec) {
        return false;
    }
    return readSmallFile(markerFor(file)) == model.sha256;
}

std::expected<std::filesystem::path, Error> ModelStore::ensure(const ModelInfo& model, IDownloadTransport* transport,
                                                               const std::function<void(const DownloadProgress&)>& progress,
                                                               std::stop_token stop) {
    if (model.kind == ModelKind::Llm && model.license != "MIT" && model.license != "Apache-2.0")
        return std::unexpected(Error{ErrorCode::InvalidArgument, "local LLM license is not allowed"});

    const std::filesystem::path file = pathFor(model);
    if (isInstalled(model)) {
        return file;
    }
    if (transport == nullptr) {
        return std::unexpected(Error{ErrorCode::NotFound, "model '" + model.id + "' is not downloaded"});
    }
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec); // the store, or a model's own directory in it
    if (ec) {
        return std::unexpected(Error{ErrorCode::IoError, "cannot create the model store " + pathToUtf8(m_root) + ": " + ec.message()});
    }
    const std::filesystem::path part = partFor(file);

    // Resume: hash what an earlier attempt already stored, then ask for the rest.
    Sha256 hash;
    std::uint64_t have = 0;
    if (std::filesystem::exists(part, ec)) {
        if (std::filesystem::file_size(part, ec) > model.size || !hashFile(part, hash, have)) {
            std::filesystem::remove(part, ec);
            hash = Sha256{};
            have = 0;
        }
    }

    std::uint64_t received = have;
    if (have < model.size) {
        std::ofstream out;
        auto openPart = [&](bool truncate) {
            out.close();
            out.clear();
            out.open(part, std::ios::binary | (truncate ? std::ios::trunc : std::ios::app));
            return static_cast<bool>(out);
        };
        if (!openPart(false)) {
            return std::unexpected(Error{ErrorCode::IoError, "cannot write " + pathToUtf8(part)});
        }
        int status = 0;
        bool tooLong = false;
        bool writeFailed = false;
        auto result = transport->get(
            model.url, have,
            [&](const IDownloadTransport::Response& response) {
                status = response.status;
                if (response.status == 200 && received > 0) {
                    // The server ignored the range and sends the whole file: start over with this response.
                    hash = Sha256{};
                    received = 0;
                    writeFailed = !openPart(true);
                    return !writeFailed;
                }
                return response.status == 200 || response.status == 206;
            },
            [&](std::span<const std::byte> data) {
                if (stop.stop_requested()) {
                    return false;
                }
                if (received + data.size() > model.size) {
                    tooLong = true;
                    return false;
                }
                out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
                if (!out) {
                    writeFailed = true;
                    return false;
                }
                hash.update(data);
                received += data.size();
                if (progress) {
                    progress({received, model.size});
                }
                return true;
            },
            stop);
        out.close();
        if (tooLong) {
            std::filesystem::remove(part, ec);
            return std::unexpected(Error{ErrorCode::InvalidArgument,
                                         "download of '" + model.id + "' is larger than the manifest says"});
        }
        if (writeFailed) {
            std::filesystem::remove(part, ec);
            return std::unexpected(Error{ErrorCode::IoError, "cannot write " + pathToUtf8(part)});
        }
        if (status != 0 && status != 200 && status != 206) {
            return std::unexpected(Error{ErrorCode::IoError,
                                         "download of '" + model.id + "' failed: HTTP " + std::to_string(status)});
        }
        if (!result) {
            return std::unexpected(result.error()); // network error or cancelled: the part stays for resuming
        }
    }
    if (received != model.size) {
        return std::unexpected(Error{ErrorCode::IoError, "download of '" + model.id + "' ended early (" +
                                                             std::to_string(received) + " of " +
                                                             std::to_string(model.size) + " bytes)"});
    }
    const std::string digest = hash.finishHex();
    if (digest != model.sha256) {
        std::filesystem::remove(part, ec);
        return std::unexpected(Error{ErrorCode::InvalidArgument, "download of '" + model.id +
                                                                     "' failed verification (SHA-256 " + digest +
                                                                     ", expected " + model.sha256 + ")"});
    }
    std::filesystem::remove(file, ec);
    std::filesystem::rename(part, file, ec);
    if (ec) {
        return std::unexpected(Error{ErrorCode::IoError, "cannot store " + pathToUtf8(file) + ": " + ec.message()});
    }
    std::ofstream(markerFor(file), std::ios::binary | std::ios::trunc) << model.sha256 << '\n';
    return file;
}

std::expected<void, Error> ModelStore::verify(const ModelInfo& model) const {
    Sha256 hash;
    std::uint64_t bytes = 0;
    if (!hashFile(pathFor(model), hash, bytes)) {
        return std::unexpected(Error{ErrorCode::NotFound, "model '" + model.id + "' is not downloaded"});
    }
    const std::string digest = hash.finishHex();
    if (bytes != model.size || digest != model.sha256) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "model '" + model.id + "' is damaged (SHA-256 " + digest + ")"});
    }
    return {};
}

std::expected<void, Error> ModelStore::remove(const ModelInfo& model) {
    std::error_code ec;
    const std::filesystem::path file = pathFor(model);
    for (const std::filesystem::path& p : {file, partFor(file), markerFor(file)}) {
        std::filesystem::remove(p, ec);
        if (ec) {
            return std::unexpected(Error{ErrorCode::IoError, "cannot remove " + pathToUtf8(p) + ": " + ec.message()});
        }
    }
    return {};
}

} // namespace wabcut::ai
