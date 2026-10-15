#pragma once

// wabcut-embed's segmentation model (AI-14; docs/ai.md "AI Object Mask"): SAM 2.1 Hiera-Tiny's video tracker as five
// ONNX graphs (square-zero-labs/sam2.1-tiny-video-onnx, exported from transformers' Sam2VideoModel):
//   vision_encoder    pixel_values [1,3,1024,1024] -> feats0, feats1, feats2 (raw), feats2_no_mem, vision_pos_embed
//   mask_decoder      feats0, feats1, feats2_cond, input_points [1,1,N,2], input_labels [1,1,N] int32
//                     -> low_res_mask, high_res_mask [1,1,1024,1024] (logits), iou [1,1], object_score_logits,
//                        object_pointer [1,1,256]
//   memory_encoder    feats2, high_res_mask, object_score_logits [1,1], binarize -> memory_tokens / memory_pos [4096,1,64]
//   memory_attention  current_vision_features / _position_embeddings [4096,1,256], memory / memory_pos [28736,1,64]
//                     -> conditioned_feats [1,256,64,64]
//   pointer_tpos      normalized_diffs [16] -> pointer_pos [16,64]
// and constants.json (normalisation, the 7 x 64 memory temporal encoding).
//
// The tracker keeps one object's memory between requests: the prompted frame's memory and object pointer, the six most
// recent tracked frames' memories and fifteen recent pointers. A prompt resets it; each tracked frame attends to it
// (memory_attention), is decoded without points, and adds its own memory. The fixed-shape graphs take exactly 7
// memories and 16 pointers: early frames pad by repeating the most recent, as the exporter validated (worst per-frame
// IoU 0.9967 against PyTorch's propagate_in_video_iterator).

#include "AudioModels.h"

#include <wabcut/ai/search/EmbedProtocol.h>

#include <onnxruntime_cxx_api.h>

#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <string>
#include <vector>

namespace wabcut::embed_segment {

class Sam2Tracker {
public:
    static constexpr int kImage = 1024;
    static constexpr int kFeat = 64;     ///< kImage / 16
    static constexpr int kTokens = kFeat * kFeat;
    static constexpr int kHidden = 256;
    static constexpr int kMemDim = 64;
    static constexpr int kMemories = 7;  ///< the prompted frame + 6 recent
    static constexpr int kPointers = 16; ///< object pointers, 4 tokens of kMemDim each

    /// Throws std::invalid_argument (a file missing) or std::runtime_error (not the expected graphs).
    Sam2Tracker(const std::filesystem::path& directory, const std::string& device, int threads, embed_audio::SessionFactory factory);

    struct Result {
        std::vector<float> logits; ///< high_res_mask, kImage x kImage, rows top-down
        float iou = 0.0f;
        float objectScore = 0.0f;
    };
    /// A new object from prompt points on `rgb` (kImage x kImage x 3, rows top-down). Throws on failure (after one retry
    /// on the CPU when the GPU failed).
    Result prompt(const std::vector<std::uint8_t>& rgb, const std::vector<ai::search::embed::SegmentPoint>& points, int frame, int frames);
    /// The next frame of the object. Throws std::logic_error before a prompt.
    Result track(const std::vector<std::uint8_t>& rgb, int frame, int frames);

    const std::string& device() const { return m_device; }
    const std::filesystem::path& directory() const { return m_directory; }

private:
    struct Memory {
        int frame = 0;
        std::vector<float> tokens; ///< kTokens x kMemDim
        std::vector<float> pos;
    };
    struct Pointer {
        int frame = 0;
        std::vector<float> value; ///< kHidden
    };
    struct Features {
        std::vector<float> feats0, feats1, feats2, feats2NoMem, pos;
    };

    void open(bool gpu);
    template <class Fn>
    Result withFallback(Fn&& fn);
    Features encode(const std::vector<std::uint8_t>& rgb);
    Result decode(const Features& f, const std::vector<float>& feats2Cond, const std::vector<ai::search::embed::SegmentPoint>& points,
                  std::vector<float>& pointer);
    Memory remember(const Features& f, const Result& r, bool prompted, int frame);
    std::vector<float> attend(const Features& f, int frame, int frames);

    std::filesystem::path m_directory;
    int m_threads = 0;
    embed_audio::SessionFactory m_factory;
    std::string m_device;
    embed_audio::Graph m_encoder, m_decoder, m_memoryEncoder, m_memoryAttention, m_pointerPe;
    std::array<float, 3> m_mean{0.485f, 0.456f, 0.406f};
    std::array<float, 3> m_std{0.229f, 0.224f, 0.225f};
    std::vector<float> m_tpos; ///< kMemories x kMemDim
    /// The last picture encoded (a hash of its pixels) and its features: refining clicks on one frame, and the prompt
    /// frame a forward and a backward pass both start from, skip the image encoder (the expensive graph).
    std::uint64_t m_encodedKey = 0;
    Features m_encoded;
    bool m_prompted = false;
    Memory m_condition;
    Pointer m_conditionPointer;
    std::deque<Memory> m_recent;         ///< most recent first, at most kMemories - 1
    std::deque<Pointer> m_recentPointers; ///< most recent first, at most kPointers - 1
};

/// The answer to a segment_frame request (Segmented or Failure), the matte written.
ai::search::embed::Response segment(const ai::search::embed::SegmentFrame& request, Sam2Tracker& model);

} // namespace wabcut::embed_segment
