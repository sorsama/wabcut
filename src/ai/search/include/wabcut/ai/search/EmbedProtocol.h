#pragma once

// The wabcut-embed protocol: newline-delimited JSON over the helper's stdin (requests) and stdout (responses), one
// object per line, UTF-8, like wabcut-infer's (wabcut/ai/InferProtocol.h). The model runs out of process, so a GPU
// driver crash or an out-of-memory in ONNX Runtime ends one request with an error and never the editor.
//
// Requests (parent -> helper):
//   {"type":"hello","protocol":1}
//   {"type":"load","id":"1","vision":"...vision.onnx","text":"...text.onnx","tokenizer":"...tokenizer.json",
//    "device":"auto","threads":0}                                  (any of vision / text+tokenizer; loads once)
//   {"type":"embed_images","id":"2","images":{"path":"...batch.rgb","width":224,"height":224,"count":16}}
//   {"type":"embed_text","id":"3","texts":["a dog on a beach"]}
//   {"type":"load","id":"4","faces":"...yunet.onnx","objects":"...yolox_tiny.onnx"}        (AI-9: detectors)
//   {"type":"detect","id":"5","images":{...},"faces":true,"objects":true,"faceThreshold":0.6,"objectThreshold":0.35}
//   {"type":"load","id":"6","speech":"...deepfilternet3 directory","stems":"...htdemucs.onnx"}  (A.4: audio models)
//   {"type":"enhance_audio","id":"7","audio":{"path":"...in.f32","channels":1,"frames":1632000,"rate":48000},
//    "output":"...out.f32"}                                   (Enhance Speech: the block enhanced, same length)
//   {"type":"separate_audio","id":"8","audio":{"path":"...","channels":2,"frames":343980,"rate":44100},
//    "output":"...out.f32"}                                   (stems: one segment, every stem after the other)
//   {"type":"load","id":"9","translation":"...opus-mt-en-mul directory"}   (AI-12: an OPUS-MT model)
//   {"type":"translate_text","id":"10","texts":["Hello."],"target":">>tha<<","maxTokens":256}
//                                                             (each text translated on its own, greedy)
//   {"type":"load","id":"11","segmentation":"...sam2.1-tiny-video directory"}   (AI-14: SAM 2.1's video tracker)
//   {"type":"segment_frame","id":"12","mode":"prompt","images":{...one 1024x1024 picture...},"frame":0,"frames":120,
//    "points":[[512,300,1],[600,800,0],[100,100,2],[900,950,3]],"output":"...matte.u8","width":512,"height":288}
//                     (prompt: clicks (1 positive, 0 negative) and a box (2 top-left, 3 bottom-right) in the picture's
//                      pixels start a new object; track: the next frame of it, from its memory; the matte, 8-bit
//                      alpha at width x height, rows top-down, is written to "output")
//   {"type":"shutdown"}
// Responses (helper -> parent), each with the id of the request it answers:
//   {"type":"hello","protocol":1,"engine":"onnxruntime","engineVersion":"1.24.4","providers":["DmlExecutionProvider",
//    "CPUExecutionProvider"]}
//   {"type":"loaded","id":"1","device":"DirectML","dimension":768}
//   {"type":"embeddings","id":"2","vectors":["<base64 float32>", ...],"ms":41.5}
//   {"type":"detections","id":"5","frames":[{"faces":[[x,y,w,h,score,l0x,l0y,...,l4y]],"objects":[[x,y,w,h,score,class]]}],
//    "ms":12.0}                          (boxes in the batch pictures' pixels; YuNet faces, YOLOX people)
//   {"type":"audio","id":"7","frames":1632000,"channels":1,"outputs":1,"ms":812.0,"device":"DirectML"}
//   {"type":"translations","id":"10","texts":["..."],"tokens":12,"ms":95.0,"device":"CPU"}
//   {"type":"segmented","id":"12","iou":0.93,"objectScore":8.2,"present":true,"width":512,"height":288,"ms":48.0,
//    "device":"DirectML"}
//   {"type":"error","id":"2","code":"inference","message":"..."}
// "device": "auto" tries the GPU provider (DirectML on Windows) and falls back to the CPU; "cpu" never touches the
// GPU. Pictures travel as one raw file of `count` frames of width x height x 3 bytes (RGB, rows top-down), already
// resized for the model; vectors come back L2-normalised (encodeVector, Embedding.h). Audio travels as raw files of
// interleaved float32 samples (AudioBlock); the answer is written to "output": the enhanced block (frames x channels)
// or the stems (outputs x frames x channels, each stem interleaved).

#include <wabcut/ai/search/Embedding.h>
#include <wabcut/model/Error.h>

#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace wabcut::ai::search::embed {

inline constexpr int kProtocolVersion = 1;
/// Bounds of one request, checked on both sides.
inline constexpr int kMaxImagesPerRequest = 256;
inline constexpr int kMaxImageSide = 1024;
inline constexpr int kMaxTextsPerRequest = 64;
inline constexpr std::size_t kMaxTextBytes = 4096;
inline constexpr std::size_t kMaxDetectionsPerFrame = 256;
inline constexpr std::size_t kFaceRow = 15;  ///< box (4), score, 5 landmarks (x, y)
inline constexpr std::size_t kObjectRow = 6; ///< box (4), score, class
inline constexpr int kMaxAudioChannels = 8;
inline constexpr int kMaxAudioFrames = 48000 * 120; ///< two minutes at 48 kHz per request
inline constexpr int kMaxAudioOutputs = 8;          ///< stems of one separation
inline constexpr int kMaxTranslationTokens = 512;   ///< generated per text (OPUS-MT's max_length)
inline constexpr int kMaxSegmentPoints = 64;        ///< prompt points (clicks and a box's corners) per frame
inline constexpr int kMaxMatteSide = 4096;          ///< a segmentation matte's width and height

// ---- Requests ----
struct Hello {
    int protocol = kProtocolVersion;
};
struct Load {
    std::string id;
    std::filesystem::path vision;    ///< image encoder (.onnx); empty = not needed
    std::filesystem::path text;      ///< text encoder (.onnx); with `tokenizer`
    std::filesystem::path tokenizer; ///< tokenizer.json
    std::string device = "auto";     ///< "auto", "gpu" or "cpu"
    int threads = 0;                 ///< CPU threads; 0 = ONNX Runtime's choice
    std::filesystem::path faces;     ///< face detector (YuNet .onnx); AI-9
    std::filesystem::path objects;   ///< object detector (YOLOX .onnx); AI-9
    std::filesystem::path speech;    ///< Enhance Speech: DeepFilterNet3's directory (enc/erb_dec/df_dec.onnx); A.4
    std::filesystem::path stems;     ///< stem separation model (HTDemucs .onnx); A.4
    /// AI-12: an OPUS-MT (Marian) translation model's directory: encoder_model_quantized.onnx,
    /// decoder_model_quantized.onnx, tokenizer.json, config.json. One at a time: loading another replaces it.
    std::filesystem::path translation;
    /// AI-14: SAM 2.1's video tracker directory: vision_encoder.onnx, mask_decoder.onnx, memory_attention.onnx,
    /// memory_encoder.onnx, pointer_tpos.onnx, constants.json.
    std::filesystem::path segmentation;
};
struct ImageBatch {
    std::filesystem::path path;
    int width = 0;
    int height = 0;
    int count = 0;
};
struct EmbedImages {
    std::string id;
    ImageBatch images;
};
struct EmbedText {
    std::string id;
    std::vector<std::string> texts;
};
struct Shutdown {};
/// Faces and people in a batch of pictures (the detectors must be loaded).
struct Detect {
    std::string id;
    ImageBatch images;
    bool faces = true;
    bool objects = true;
    float faceThreshold = 0.6f;
    float objectThreshold = 0.35f;
    bool allClasses = false; ///< every COCO class (AI-14's object prompts: "the red car"), else people only
};
/// A raw file of `frames` x `channels` interleaved float32 samples at `rate` Hz.
struct AudioBlock {
    std::filesystem::path path;
    int channels = 0;
    int frames = 0;
    int rate = 0;
};
/// Enhance Speech over one block (the speech model must be loaded): the block's enhanced samples, same length and
/// layout, written to `output`. Each channel is enhanced on its own.
struct EnhanceAudio {
    std::string id;
    AudioBlock audio;
    std::filesystem::path output;
};
/// Stem separation of one segment (the stem model must be loaded): every stem's samples, the stems one after the
/// other, written to `output`.
struct SeparateAudio {
    std::string id;
    AudioBlock audio;
    std::filesystem::path output;
};

/// Translation (the translation model must be loaded): each text on its own, greedy decoding of at most `maxTokens`
/// tokens. `target` is the target-language token a multilingual model needs (">>tha<<"), else empty.
struct TranslateText {
    std::string id;
    std::vector<std::string> texts;
    std::string target;
    int maxTokens = 256;
};

/// A prompt point in the picture's pixels: label 1 positive click, 0 negative click, 2 / 3 a box's top-left /
/// bottom-right corner.
struct SegmentPoint {
    float x = 0.0f;
    float y = 0.0f;
    int label = 1;
};
/// AI-14 segmentation (the segmentation model must be loaded): one picture of an object's video, at the model's input
/// size (SAM 2.1: 1024 x 1024, the frame stretched). "prompt" starts a new object from `points` on this picture (its
/// memory is reset); "track" segments the next picture of the same object from its memory (`points` empty). `frame` is
/// the picture's index in its run and `frames` the run's length (the tracker's temporal positions). The matte (8-bit
/// alpha, `width` x `height`, rows top-down) is written to `output`.
struct SegmentFrame {
    std::string id;
    std::string mode = "prompt"; ///< "prompt" or "track"
    ImageBatch images;           ///< exactly one picture
    int frame = 0;
    int frames = 1;
    std::vector<SegmentPoint> points;
    std::filesystem::path output;
    int width = 0;
    int height = 0;
};

using Request = std::variant<Hello, Load, EmbedImages, EmbedText, Shutdown, Detect, EnhanceAudio, SeparateAudio, TranslateText, SegmentFrame>;

// ---- Responses ----
struct HelloReply {
    int protocol = 0;
    std::string engine;
    std::string engineVersion;
    std::vector<std::string> providers;
};
struct Loaded {
    std::string id;
    std::string device; ///< what the models run on: "DirectML", "CPU", "CoreML"
    int dimension = 0;  ///< of the embedding models; 0 when only detectors were loaded
};
struct Embeddings {
    std::string id;
    std::vector<Vector> vectors;
    double milliseconds = 0.0; ///< inference time in the helper
};
struct Failure {
    std::string id;
    std::string code; ///< "bad_request", "model", "io", "inference", "protocol"
    std::string message;
};

/// One picture's detections: rows of kFaceRow / kObjectRow numbers.
struct DetectedFrame {
    std::vector<std::vector<float>> faces;
    std::vector<std::vector<float>> objects;
};
struct Detections {
    std::string id;
    std::vector<DetectedFrame> frames;
    double milliseconds = 0.0;
};
/// An audio request done: `outputs` blocks of `frames` x `channels` samples are in its output file.
struct AudioDone {
    std::string id;
    int frames = 0;
    int channels = 0;
    int outputs = 0;
    double milliseconds = 0.0;
    std::string device; ///< what it ran on ("DirectML", "CPU")
};

/// A translate_text request done: the translations in request order.
struct Translations {
    std::string id;
    std::vector<std::string> texts;
    int tokens = 0;            ///< generated, in total
    double milliseconds = 0.0;
    std::string device;
};

/// A segment_frame request done: the matte is in its output file.
struct Segmented {
    std::string id;
    float iou = 0.0f;         ///< the decoder's predicted IoU of the matte (its confidence)
    float objectScore = 0.0f; ///< > 0: the object is in the picture (the matte is empty otherwise)
    bool present = false;
    int width = 0;
    int height = 0;
    double milliseconds = 0.0;
    std::string device;
};

using Response = std::variant<HelloReply, Loaded, Embeddings, Failure, Detections, AudioDone, Translations, Segmented>;

std::string encode(const Request& request);
std::string encode(const Response& response);

/// Errors: ParseError (not JSON, unknown type, wrong or missing members, a bound exceeded).
std::expected<Request, Error> decodeRequest(std::string_view line);
std::expected<Response, Error> decodeResponse(std::string_view line);

/// The request id a response answers ("" for HelloReply).
const std::string& responseId(const Response& response);

/// Writes pictures of one size as an ImageBatch file. Errors: InvalidArgument (mixed sizes, invalid pictures, out
/// of bounds), IoError.
std::expected<ImageBatch, Error> writeImageBatch(const std::filesystem::path& path, std::span<const Image> images);
/// Reads one back. Errors: InvalidArgument (out of bounds), IoError (missing or short file).
std::expected<std::vector<Image>, Error> readImageBatch(const ImageBatch& batch);

/// Writes interleaved samples as an AudioBlock file. Errors: InvalidArgument (out of bounds, not whole frames),
/// IoError.
std::expected<AudioBlock, Error> writeAudioBlock(const std::filesystem::path& path, std::span<const float> samples,
                                                 int channels, int rate);
/// Reads `blocks` consecutive blocks of `block`'s size from its file (1 for an input, the stems for a separation).
/// Errors: InvalidArgument (out of bounds), IoError (missing or short file).
std::expected<std::vector<float>, Error> readAudioBlock(const AudioBlock& block, int blocks = 1);

} // namespace wabcut::ai::search::embed
