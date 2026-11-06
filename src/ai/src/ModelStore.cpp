#include <wabcut/ai/ModelStore.h>

#include "TranslationManifest.h"

#include <wabcut/ai/Sha256.h>
#include <wabcut/model/MediaLocator.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <mutex>
#include <set>
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

/// AI video effects (0.7, docs/ai.md "AI video effects"): image-to-image ONNX models, each pinned to a Hugging Face revision;
/// sizes and SHA-256 are the repositories' LFS metadata (checked 2026-11-05, and the digests re-computed on the downloaded
/// files). Real-ESRGAN (Xintao Wang et al., BSD-3-Clause) in two re-exports: realesr-general-x4v3 (a compact 4x upscaler) by
/// Heliosoph, RealESRGAN_x2plus / x4plus (RRDBNet) by SceneWorks. Deblur: NAFNet (Megvii, MIT) GoPro, as the OpenCV Zoo
/// publishes it. Denoise: DRUNet (Kai Zhang, KAIR, MIT), colour, with a noise-level channel, exported by synthscript.
/// All take 1 x 3 x H x W float32 RGB in [0, 1] (DRUNet 4 channels: the noise level over 255 as the fourth) and have
/// dynamic H and W.
constexpr std::string_view kVideoRealGeneralRepo = "Heliosoph/realesrgan-onnx";
constexpr std::string_view kVideoRealGeneralRevision = "488e5dda07333179f229a6205d92135eea4c25e9";
constexpr std::string_view kVideoRealPlusRepo = "SceneWorks/real-esrgan-onnx";
constexpr std::string_view kVideoRealPlusRevision = "09f741bac80a246b407da3ee902bf5f3291b602f";
constexpr std::string_view kVideoNafnetRepo = "opencv/deblurring_nafnet";
constexpr std::string_view kVideoNafnetRevision = "f1f255116cdb628a311d2b5749871189a4639d84";
constexpr std::string_view kVideoDrunetRepo = "synthscript/drunet-color-onnx";
constexpr std::string_view kVideoDrunetRevision = "a2b9fccfa27b197f44a3876c567f5e48970c44a7";
constexpr std::string_view kRealEsrganCard =
    "Real-ESRGAN (Xintao Wang, Liangbin Xie, Chao Dong, Ying Shan; Tencent ARC Lab), BSD 3-Clause License (Copyright (c) "
    "2021, Xintao Wang), converted to ONNX without changing the weights. AI Restore runs it on this computer, picture by "
    "picture: no picture leaves it. https://github.com/xinntao/Real-ESRGAN/blob/master/LICENSE";
constexpr std::string_view kNafnetCard =
    "NAFNet GoPro deblurring (Liangyu Chen, Xiaojie Chu, Xiangyu Zhang, Jian Sun; Megvii), MIT License (Copyright (c) 2022 "
    "megvii-model), as the OpenCV Zoo publishes the ONNX export. AI Restore runs it on this computer: no picture leaves "
    "it. https://github.com/megvii-research/NAFNet/blob/main/LICENSE";
constexpr std::string_view kDrunetCard =
    "DRUNet colour denoiser (Kai Zhang, Yawei Li, Wangmeng Zuo, Lei Zhang, Luc Van Gool, Radu Timofte; KAIR), MIT License "
    "(Copyright (c) 2019 Kai Zhang), original weights exported to ONNX unchanged. It was trained on public image collections "
    "published for research (DIV2K, Flickr2K, Waterloo Exploration); the author's licence for the weights is MIT. AI Denoise "
    "and AI Restore run it on this computer: no picture leaves it. https://github.com/cszn/KAIR/blob/master/LICENSE";

/// AI frame interpolation and AI refocus (0.7; verified 2026-11-05: repository, revision, file, size, SHA-256 re-computed on the
/// downloaded files, licence from the model cards, graph inputs and outputs read with ONNX Runtime 1.31).
///  * RIFE 4.25 lite (Practical-RIFE by hzwer, MIT) exported to ONNX by t9749 with the export scripts in the repository
///    (weights loaded tensors-only; the one change to the network is a stateless warp): inputs img0 and img1 (1 x 3 x H x W
///    float32, RGB 0..1; H and W multiples of 64, dynamic) and timestep (1 x 1 x 1 x 1 float32, 0..1), output frame (1 x 3 x H x W).
///  * Depth Anything V2 Small (Lihe Yang et al., Apache-2.0; the Base, Large and Giant models are CC-BY-NC and are not used) as
///    onnx-community publishes it: input pixel_values (batch x 3 x H x W float32, ImageNet mean and standard deviation, H and W
///    multiples of 14), output predicted_depth (batch x 14*floor(H/14) x 14*floor(W/14)): relative inverse depth, larger = nearer.
constexpr std::string_view kVideoRifeRepo = "t9749/rife-4.25-lite-onnx";
constexpr std::string_view kVideoRifeRevision = "8d1488a82e19fd2f2ca5b3ee1d1bfacd6787b9df";
constexpr std::string_view kVideoDepthRepo = "onnx-community/depth-anything-v2-small";
constexpr std::string_view kVideoDepthRevision = "4472b7362082ad9968fee890ca0f1e5aca36b93d";
constexpr std::string_view kRifeCard =
    "RIFE 4.25 lite (Zhewei Huang, Tianyuan Zhang, Wen Heng, Boxin Shi, Shuchang Zhou; Practical-RIFE by hzwer), MIT License "
    "(Copyright (c) 2021 hzwer), the author's own 4.25 lite weights converted to ONNX without retraining. AI Frame "
    "Interpolation runs it on this computer, picture pair by picture pair: no picture leaves it. "
    "https://github.com/hzwer/Practical-RIFE/blob/main/LICENSE";
constexpr std::string_view kDepthCard =
    "Depth Anything V2 Small (Lihe Yang, Bingyi Kang, Zilong Huang, Zhen Zhao, Xiaogang Xu, Jiashi Feng, Hengshuang Zhao), "
    "Apache License 2.0, as ONNX weights published by the ONNX Community. AI Refocus runs it on this computer to estimate "
    "how far each part of the picture is: no picture leaves it. https://huggingface.co/depth-anything/Depth-Anything-V2-Small";

/// AI Face Retouch (0.7; verified 2026-11-05): BiSeNet face parsing, ResNet-18 backbone, by Yakhyokhuja Valikhujaev
/// (github.com/yakhyo/face-parsing, MIT, built on zllrunning/face-parsing.PyTorch, MIT), the author's trained weights exported to ONNX and
/// published as a release asset (tag v0.0.2; the SHA-256 is the GitHub release digest and was re-computed on the download). Graph read
/// with ONNX Runtime 1.31: input `input` 1 x 3 x 512 x 512 float32, RGB 0..1 with the ImageNet mean (.485 .456 .406) and deviation
/// (.229 .224 .225); outputs `output` (and two auxiliary heads, 414 and 424) 1 x 19 x 512 x 512 class logits, the 19 CelebAMask-HQ
/// classes (0 background, 1 skin, 2 l_brow, 3 r_brow, 4 l_eye, 5 r_eye, 6 eye_g, 7 l_ear, 8 r_ear, 9 ear_r, 10 nose, 11 mouth,
/// 12 u_lip, 13 l_lip, 14 neck, 15 neck_l, 16 cloth, 17 hair, 18 hat). The input size is fixed.
constexpr std::string_view kFaceParsingUrl = "https://github.com/yakhyo/face-parsing/releases/download/v0.0.2/resnet18.onnx";
constexpr std::string_view kFaceParsingCard =
    "BiSeNet face parsing, ResNet-18 (Yakhyokhuja Valikhujaev; architecture and training recipe from zllrunning's face-parsing."
    "PyTorch, also MIT), MIT License (Copyright (c) 2024 Yakhyokhuja Valikhujaev), the author's trained weights exported to ONNX. "
    "It was trained on CelebAMask-HQ, a face-parsing image collection published for research. AI Face Retouch runs it on this "
    "computer, on each face the YuNet detector finds: no picture leaves it. https://github.com/yakhyo/face-parsing/blob/main/LICENSE";

/// AI Eye Contact (0.7; verified 2026-11-05): the MediaPipe Face Landmarker's landmark network (Google LLC, Apache-2.0: the face mesh with
/// iris points), converted from TFLite to ONNX by naklitechie (format only, no retraining; the repository carries MediaPipe's Apache-2.0
/// LICENSE and its model card links Google's Face Mesh V2 model card), pinned to the revision checked. The SHA-256 is the one the
/// repository's model card states and was re-computed on the download. Graph read with ONNX Runtime 1.31: input `input_12` 1 x 256 x 256 x 3
/// float32 (NHWC) RGB 0..1, a face crop rolled so the eyes are level; outputs `Identity` 1 x 1 x 1 x 1434 = 478 x (x, y, z) in input pixels
/// (the face mesh and ten iris points: 468..472 and 473..477), `Identity_1` the face-presence logit, `Identity_2` auxiliary. No gaze-redirection
/// network with a licence that allows this use was found (searched 2026-11-05): the landmarks measure the eyes, the effect moves the iris.
constexpr std::string_view kFaceLandmarksRepo = "naklitechie/face-landmarks-onnx";
constexpr std::string_view kFaceLandmarksRevision = "575c33816c840c5b156398adb485e4f8d138adc2";
constexpr std::string_view kFaceLandmarksCard =
    "MediaPipe Face Landmarker landmark network (Google LLC), Apache License 2.0, as ONNX weights converted from the TFLite model by "
    "naklitechie (format change only). It places 478 points on a face, the eyelids and the irises among them. AI Eye Contact runs it on "
    "this computer, on each face the YuNet detector finds, to measure where the irises are: no picture leaves it. "
    "https://huggingface.co/naklitechie/face-landmarks-onnx";

ModelInfo videoModel(std::string id, std::string file, std::string_view repo, std::string_view revision, std::string_view repoPath,
                     std::uint64_t size, std::string sha, std::string license, std::string_view card, std::string description) {
    return ModelInfo{std::move(id), ModelKind::Video, "ai-video/" + std::move(file), hfUrl(repo, revision, repoPath), size, std::move(sha),
                     std::move(license), std::string(card), std::move(description)};
}

std::vector<ModelInfo> videoManifest() {
    return {
        videoModel("video-upscale-general-x4v3", "realesr-general-x4v3.onnx", kVideoRealGeneralRepo, kVideoRealGeneralRevision,
                   "realesr-general-x4v3.onnx", 4871181, "09b757accd747d7e423c1d352b3e8f23e77cc5742d04bae958d4eb8082b76fa4",
                   "BSD-3-Clause", kRealEsrganCard, "Real-ESRGAN general x4 v3 (5 MB): the fast upscaler, 4x (2x by halving it)"),
        videoModel("video-upscale-x2plus", "real_esrgan_x2.onnx", kVideoRealPlusRepo, kVideoRealPlusRevision, "real_esrgan_x2.onnx",
                   67073434, "7115ba92e8a1bfa63d68558ef006ef3d91273a068d321b1439f8bb1c9179002c", "BSD-3-Clause", kRealEsrganCard,
                   "Real-ESRGAN x2plus (67 MB): the high quality 2x upscaler"),
        videoModel("video-upscale-x4plus", "real_esrgan_x4.onnx", kVideoRealPlusRepo, kVideoRealPlusRevision, "real_esrgan_x4.onnx",
                   67051616, "5c586662929cbc686c1a5c38d9c060dbdb4ea5863a1f7672b8c0761e6b89c033", "BSD-3-Clause", kRealEsrganCard,
                   "Real-ESRGAN x4plus (67 MB): the high quality 4x upscaler"),
        videoModel("video-deblur-nafnet", "deblurring_nafnet_2025may.onnx", kVideoNafnetRepo, kVideoNafnetRevision,
                   "deblurring_nafnet_2025may.onnx", 91736251, "07263f416febecce10193dd648e950b22e397cf521eedab1a114ef77b2bc9587", "MIT",
                   kNafnetCard, "NAFNet deblurring (92 MB): sharpens motion blur and soft focus"),
        videoModel("video-denoise-drunet", "drunet_color.onnx", kVideoDrunetRepo, kVideoDrunetRevision, "drunet_color.onnx", 130589232,
                   "2ae3ab5eb15daac2ee79be984d584b908ce7f0f60b27be87d005f728c2aa0087", "MIT", kDrunetCard,
                   "DRUNet colour denoiser (131 MB): removes noise at a level you choose"),
        videoModel("video-interpolate-rife", "rife_v425lite.onnx", kVideoRifeRepo, kVideoRifeRevision, "rife_v425lite.onnx", 22677513,
                   "702802d0862ab78f2d72133c6bc3768e7e89b81756f7cd51d1055a2fe3d06577", "MIT", kRifeCard,
                   "RIFE 4.25 lite (23 MB): makes the frames between two frames for smooth slow motion"),
        videoModel("video-depth-anything-v2-small", "depth_anything_v2_small.onnx", kVideoDepthRepo, kVideoDepthRevision,
                   "onnx/model.onnx", 99060839, "afb6a5c28f3b6bf1618c6e43f02073ef9dfdc70e937502d51603e57b0a1df10c", "Apache-2.0", kDepthCard,
                   "Depth Anything V2 Small (99 MB): estimates how far each part of a picture is, for AI Refocus"),
        ModelInfo{"video-face-parsing-bisenet", ModelKind::Video, "ai-video/face-parsing-bisenet-resnet18.onnx", std::string(kFaceParsingUrl),
                  53205364, "0d9bd318e46987c3bdbfacae9e2c0f461cae1c6ac6ea6d43bbe541a91727e33f", "MIT", std::string(kFaceParsingCard),
                  "BiSeNet face parsing (53 MB): tells skin from eyes, mouth, hair and background in each face, for AI Face Retouch"},
        videoModel("video-face-landmarks", "face-landmarks-mediapipe.onnx", kFaceLandmarksRepo, kFaceLandmarksRevision, "face_landmarks.onnx",
                   4920995, "f38c3321ceffbc9e95103480ad38cc3f52e7e1bde2bcee7cd9355d0b9138ac0c", "Apache-2.0", kFaceLandmarksCard,
                   "Face landmarks (5 MB): finds the eyelids and irises of each face, for AI Eye Contact"),
    };
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
        // r07, the model library's transcription picker: more of the same repository revision (sizes and SHA-256 from
        // its LFS metadata, read 2026-11-03 through /api/models/ggerganov/whisper.cpp/tree/<revision>).
        whisper("base.en", "ggml-base.en.bin", 147964211, "a03779c86df3323075f5e796cb2ce5029f00ec8869eee3fdfb897afe36c6d002",
                "Whisper base, English only (148 MB): a little more accurate than base for English"),
        whisper("small", "ggml-small.bin", 487601967, "1be3a9b2063867b937e64e2ec7483364a79917e157fa98c5d94b5c1fffea987b",
                "Whisper small, multilingual (488 MB): clearly better than base, still fine on a CPU"),
        whisper("small.en", "ggml-small.en.bin", 487614201, "c6138d6d58ecc8322097e0f987c32f1be8bb0a18532a3f88f734d1bbf9c41e5d",
                "Whisper small, English only (488 MB)"),
        whisper("medium-q5_0", "ggml-medium-q5_0.bin", 539212467,
                "19fea4b380c3a618ec4723c3eef2eb785ffba0d0538cf43f8f235e7b3b34220f",
                "Whisper medium, multilingual, 5-bit (539 MB): slower than large-v3-turbo for similar quality"),
        whisper("large-v3-turbo-q8_0", "ggml-large-v3-turbo-q8_0.bin", 874188075,
                "317eb69c11673c9de1e1f0d459b253999804ec71ac4c23c17ecf5fbe24e259a1",
                "Whisper large-v3-turbo, 8-bit (874 MB): closer to the full model, for a GPU"),
        whisper("large-v3-turbo-f16", "ggml-large-v3-turbo.bin", 1624555275,
                "1fc70f774d38eb169993ac391eea357ef47c88757ef72ee5943879b7e8e2bc69",
                "Whisper large-v3-turbo, full precision (1.6 GB): for a GPU with memory to spare"),
        whisper("large-v3-q5_0", "ggml-large-v3-q5_0.bin", 1081140203,
                "d75795ecff3f83b5faa89d1900604ad8c780abd5739fae406de19f23ecd98ad1",
                "Whisper large-v3, 5-bit (1.1 GB): the most accurate, much slower than turbo; for a GPU"),
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
        for (ModelInfo& m : videoManifest()) {
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

/// The model library's registered models (r07). A deque: entries never move, so pointers stay valid; a new entry
/// for an id that exists is appended and found first (the newest wins); unregistering hides the id.
struct Registered {
    std::mutex mutex;
    std::deque<ModelInfo> models;
    std::set<std::string, std::less<>> hidden;
};
Registered& registered() {
    static Registered r;
    return r;
}

/// A store-relative file name that stays inside the store: relative, '/'-separated, no "." or ".." parts, no drive,
/// colon, backslash or control character.
bool safeStoreName(std::string_view name) {
    if (name.empty() || name.front() == '/' || name.back() == '/' || name.size() > 400)
        return false;
    for (const char c : name) {
        if (static_cast<unsigned char>(c) < 0x20 || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|')
            return false;
    }
    std::size_t start = 0;
    while (start <= name.size()) {
        const std::size_t slash = name.find('/', start);
        const std::string_view part = name.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
        if (part.empty() || part == "." || part == "..")
            return false;
        if (slash == std::string_view::npos)
            break;
        start = slash + 1;
    }
    return true;
}

std::mutex& rootMutex() {
    static std::mutex m;
    return m;
}
std::filesystem::path& rootOverride() {
    static std::filesystem::path p;
    return p;
}

} // namespace

std::span<const ModelInfo> builtinModels() { return manifest(); }

bool registerModel(ModelInfo model) {
    if (model.id.empty() || !safeStoreName(model.fileName) || model.sha256.size() != 64)
        return false;
    for (const ModelInfo& m : manifest()) {
        if (m.id == model.id)
            return false; // built-in ids stay built-in
    }
    Registered& r = registered();
    std::lock_guard lock(r.mutex);
    if (auto hidden = r.hidden.find(model.id); hidden != r.hidden.end())
        r.hidden.erase(hidden);
    for (auto it = r.models.rbegin(); it != r.models.rend(); ++it) {
        if (it->id == model.id) {
            if (it->fileName == model.fileName && it->sha256 == model.sha256 && it->size == model.size &&
                it->parts == model.parts && it->projector == model.projector &&
                it->licenseAccepted == model.licenseAccepted && it->paramsB == model.paramsB &&
                it->description == model.description)
                return true; // the same entry again
            break;
        }
    }
    r.models.push_back(std::move(model));
    return true;
}

void unregisterModel(std::string_view id) {
    Registered& r = registered();
    std::lock_guard lock(r.mutex);
    r.hidden.insert(std::string(id));
}

std::vector<ModelInfo> registeredModels() {
    Registered& r = registered();
    std::lock_guard lock(r.mutex);
    std::vector<ModelInfo> out;
    std::set<std::string, std::less<>> seen;
    for (auto it = r.models.rbegin(); it != r.models.rend(); ++it) {
        if (r.hidden.contains(it->id) || !seen.insert(it->id).second)
            continue;
        out.push_back(*it);
    }
    std::reverse(out.begin(), out.end());
    return out;
}

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
    Registered& r = registered();
    std::lock_guard lock(r.mutex);
    if (r.hidden.contains(id)) {
        return nullptr;
    }
    for (auto it = r.models.rbegin(); it != r.models.rend(); ++it) {
        if (it->id == id) {
            return &*it;
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

std::filesystem::path ModelStore::defaultRoot() {
    {
        std::lock_guard lock(rootMutex());
        if (!rootOverride().empty()) {
            return rootOverride();
        }
    }
    return userCacheDir() / "models";
}

void ModelStore::setDefaultRoot(std::filesystem::path root) {
    std::lock_guard lock(rootMutex());
    rootOverride() = std::move(root);
}

std::uint64_t ModelStore::partialBytes(const ModelInfo& model) const {
    std::error_code ec;
    const std::filesystem::path part = partFor(pathFor(model));
    const std::uint64_t size = std::filesystem::is_regular_file(part, ec) ? std::filesystem::file_size(part, ec) : 0;
    return ec ? 0 : size;
}

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
    // A library model with another licence downloads only after the user accepted it (r07, licenseAccepted).
    if (model.kind == ModelKind::Llm && model.license != "MIT" && model.license != "Apache-2.0" && !model.licenseAccepted)
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
