#pragma once

// The wabcut-infer protocol: newline-delimited JSON over the helper's stdin (requests) and stdout (responses),
// one object per line, UTF-8, every object with a "type". Heavy inference runs out of process so a GPU driver
// crash or an out-of-memory never takes the editor down (docs/ai.md); the parent kills the helper on cancel,
// timeout or protocol violation.
//
// Requests (parent -> helper):
//   {"type":"hello","protocol":1}
//   {"type":"load_model","id":"1","path":"...ggml-base.bin","useGpu":true,"gpuDevice":0}
//   {"type":"load_vad","id":"2","path":"...ggml-silero-v6.2.0.bin"}
//   {"type":"vad","id":"3","pcm":{"path":"...pcm.f32","offset":0,"count":160000},"threshold":0.5,
//    "minSpeechMs":250,"minSilenceMs":100,"padMs":30}
//   {"type":"transcribe","id":"4","pcm":{...},"language":"auto","threads":4,"beamSize":5,"vad":true,
//    "initialPrompt":""}
//   {"type":"cancel","id":"4"}     (ends the running request with error "cancelled")
//   {"type":"shutdown"}
// Responses (helper -> parent), each carrying the id of the request it answers:
//   {"type":"hello","protocol":1,"engine":"whisper.cpp","engineVersion":"1.9.5","devices":[{"name":"Vulkan0",
//    "description":"NVIDIA GeForce RTX 3060","kind":"gpu"}]}
//   {"type":"loaded","id":"1","device":"Vulkan0","info":"base, multilingual"}
//   {"type":"progress","id":"4","percent":37}
//   {"type":"segment","id":"4","language":"en","t0":0,"t1":250,"speakerTurnNext":false,
//    "tokens":[{"b":"2048656c6c6f","t0":0,"t1":40,"p":0.98}]}          (partial result, as decoded)
//   {"type":"vad_result","id":"3","segments":[[12,250],[300,410]]}
//   {"type":"result","id":"4","language":"en","segments":12,"stats":{"encodeMs":800}}
//   {"type":"error","id":"4","code":"cancelled","message":"..."}
// PCM is a raw little-endian float32, 16 kHz mono file; "offset"/"count" are in samples. Times are centiseconds
// from the first sample of the request's PCM range. Token bytes travel as hex ("b") because a byte-level BPE token
// may end inside a UTF-8 character, which a JSON string cannot carry.

#include <wabcut/ai/WordBuilder.h>
#include <wabcut/model/Error.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace wabcut::ai::infer {

inline constexpr int kProtocolVersion = 1;

struct PcmRange {
    std::filesystem::path path;
    std::int64_t offset = 0; ///< samples
    std::int64_t count = 0;  ///< samples
};

// ---- Requests ----
struct Hello {
    int protocol = kProtocolVersion;
};
struct LoadModel {
    std::string id;
    std::filesystem::path path;
    bool useGpu = true;
    int gpuDevice = 0;
};
struct LoadVad {
    std::string id;
    std::filesystem::path path;
};
struct VadRequest {
    std::string id;
    PcmRange pcm;
    float threshold = 0.5f;
    int minSpeechMs = 250;
    int minSilenceMs = 100;
    int padMs = 30;
};
struct TranscribeRequest {
    std::string id;
    PcmRange pcm;
    std::string language = "auto";
    int threads = 4;
    int beamSize = 5; ///< 1 = greedy
    bool vad = false; ///< run whisper's own VAD inside the range (needs load_vad first)
    std::string initialPrompt;
};
struct Cancel {
    std::string id;
};
struct Shutdown {};

using Request = std::variant<Hello, LoadModel, LoadVad, VadRequest, TranscribeRequest, Cancel, Shutdown>;

// ---- Responses ----
struct DeviceInfo {
    std::string name;
    std::string description;
    std::string kind; ///< "cpu", "gpu", "igpu", "accel"
};
struct HelloReply {
    int protocol = 0;
    std::string engine;
    std::string engineVersion;
    std::vector<DeviceInfo> devices;
};
struct Loaded {
    std::string id;
    std::string device; ///< backend the model runs on
    std::string info;
};
struct Progress {
    std::string id;
    int percent = 0;
};
struct Segment {
    std::string id;
    std::string language; ///< language the decoder is using ("" if unknown)
    RecognizedSegment segment;
};
struct VadResult {
    std::string id;
    std::vector<std::pair<std::int64_t, std::int64_t>> segments; ///< centiseconds
};
struct Result {
    std::string id;
    std::string language;
    int segments = 0;
    std::map<std::string, double> stats;
};
struct Failure {
    std::string id;
    std::string code; ///< "bad_request", "model", "io", "inference", "cancelled", "protocol"
    std::string message;
};

using Response = std::variant<HelloReply, Loaded, Progress, Segment, VadResult, Result, Failure>;

/// One line of JSON, without the newline.
std::string encode(const Request& request);
std::string encode(const Response& response);

/// Parses one line. Errors: ParseError (not JSON, unknown type, wrong or missing members).
std::expected<Request, Error> decodeRequest(std::string_view line);
std::expected<Response, Error> decodeResponse(std::string_view line);

/// The request id of a response ("" for HelloReply).
const std::string& responseId(const Response& response);

std::string toHex(std::string_view bytes);
std::optional<std::string> fromHex(std::string_view hex);

} // namespace wabcut::ai::infer
