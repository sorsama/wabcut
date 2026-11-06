#pragma once

// wabcut-embed's image-to-image models (0.7; docs/ai.md "AI video effects"): Real-ESRGAN, NAFNet deblurring and DRUNet
// denoising, each one ONNX graph with one input and one output (NCHW, RGB in [0, 1]; DRUNet's input has a fourth channel,
// the noise level over 255). A picture goes through it in overlapping tiles of one size (ai/video/Tiling.h), so DirectML
// compiles the graph once and memory stays bounded by the tile. fp32 or fp16 graphs both work: the tile is converted to
// the input's element type and the output back.

#include "AudioModels.h"

#include <wabcut/ai/search/EmbedProtocol.h>
#include <wabcut/model/Error.h>

#include <onnxruntime_cxx_api.h>

#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace wabcut::embed_picture {

/// What a model does with its graph: the image-to-image models take one picture in tiles; frame interpolation (RIFE) takes two
/// pictures and a timestep; depth estimation (Depth Anything) takes one picture and makes a one-channel map. These run on the whole
/// picture. AI Face Retouch uses two: YuNet (FaceDetect: finds the faces of a picture) and BiSeNet (FaceParse: labels the pixels of
/// a 512 x 512 face crop); a process_picture request for the parser also uses the detector, which must be loaded too.
enum class PictureKind { Image, Interpolate, Depth, FaceDetect, FaceParse, FaceLandmarks };
PictureKind pictureKindOf(const std::string& key);

class PictureModel {
public:
    /// Throws std::invalid_argument (the file is missing) or std::runtime_error (not the graph the key names).
    PictureModel(std::string key, const std::filesystem::path& file, const std::string& device, int threads,
                 embed_audio::SessionFactory factory);

    const std::string& key() const { return m_key; }
    const std::string& device() const { return m_device; }
    PictureKind kind() const { return m_kind; }
    /// 3 or 4: the input's channels (the fourth is the noise level).
    int inputChannels() const { return m_channels; }

    /// Interpolation: `a` and `b` are 3 x height x width floats (planar RGB, 0..1; both sides multiples of 64); `out` receives
    /// the frame at `timestep` (0 = a, 1 = b), the same layout. GPU failure: the CPU, once.
    std::expected<void, Error> interpolate(std::span<const float> a, std::span<const float> b, int width, int height, float timestep,
                                           std::vector<float>& out);
    /// Depth: `input` is 3 x height x width floats, planar RGB already normalised with the ImageNet mean and deviation (both
    /// sides multiples of 14); `out` receives height x width floats of relative inverse depth (larger = nearer).
    std::expected<void, Error> depth(std::span<const float> input, int width, int height, std::vector<float>& out);

    /// FaceDetect: `input` is YuNet's 3 x 640 x 640 floats (planar BGR, 0..255); `outputs` receive the graph's outputs by name
    /// (cls_8, obj_8, bbox_8, kps_8, and the same for 16 and 32), each flattened.
    std::expected<void, Error> detectFaces(std::span<const float> input, std::vector<std::pair<std::string, std::vector<float>>>& outputs);
    /// FaceParse: `input` is 3 x 512 x 512 floats (planar RGB, ImageNet-normalised); `logits` receive 19 x 512 x 512 class scores.
    std::expected<void, Error> parseFace(std::span<const float> input, std::vector<float>& logits);
    /// FaceLandmarks (AI Eye Contact, MediaPipe's face-mesh network): `input` is 256 x 256 x 3 floats (NHWC, RGB 0..1, a face crop rolled so
    /// the eyes are level); `landmarks` receive 478 x (x, y, z) in input pixels and `presenceLogit` the face-presence logit.
    std::expected<void, Error> landmarkFace(std::span<const float> input, std::vector<float>& landmarks, float& presenceLogit);

    /// One tile: `input` is 3 x size x size floats (planar RGB, 0..1); `output` receives 3 x (size x scale)^2 floats. On a
    /// GPU failure the graph is reloaded on the CPU and the tile run again. Errors carry the message.
    std::expected<void, Error> run(std::span<const float> input, int size, double sigma, int scale, std::vector<float>& output);

private:
    std::unique_ptr<Ort::Session> open(bool gpu);
    std::expected<void, Error> runOnce(std::span<const float> input, int size, double sigma, int scale, std::vector<float>& output);
    std::expected<void, Error> interpolateOnce(std::span<const float> a, std::span<const float> b, int width, int height, float timestep,
                                               std::vector<float>& out);
    std::expected<void, Error> depthOnce(std::span<const float> input, int width, int height, std::vector<float>& out);
    std::expected<void, Error> detectOnce(std::span<const float> input, std::vector<std::pair<std::string, std::vector<float>>>& outputs);
    std::expected<void, Error> parseOnce(std::span<const float> input, std::vector<float>& logits);
    std::expected<void, Error> landmarkOnce(std::span<const float> input, std::vector<float>& landmarks, float& presenceLogit);
    /// `once()` on the current session; if it fails on the GPU, the graph is reloaded on the CPU and `once()` run again.
    template <class F>
    std::expected<void, Error> withCpuFallback(F&& once);

    PictureKind m_kind = PictureKind::Image;
    std::vector<std::string> m_inputNames; ///< all of the graph's inputs, in order (interpolation: img0, img1, timestep)
    std::string m_key;
    std::filesystem::path m_file;
    int m_threads = 0;
    embed_audio::SessionFactory m_factory;
    std::unique_ptr<Ort::Session> m_session;
    std::string m_inputName;
    std::string m_outputName;
    std::vector<std::string> m_outputNames; ///< all of the graph's outputs (the face detector has twelve)
    std::string m_device;
    int m_channels = 3;
    bool m_half = false;
    Ort::MemoryInfo m_memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
};

/// process_picture over `model`: reads the input file, tiles, writes the output file. The reply carries the output's size,
/// the tile count and the time; failures are a Failure ("io", "inference", "bad_request").
ai::search::embed::Response process(const ai::search::embed::ProcessPicture& request, PictureModel& model);

/// process_picture for the face parser: `detector` finds the faces of the input picture, each face's crop goes through `parser`, and the
/// answer is the face map (R skin, G mouth, B eyes coverage; ai/video/FaceParse.h) at the input's size. `tiles` in the reply is the
/// number of faces looked at.
ai::search::embed::Response processFaces(const ai::search::embed::ProcessPicture& request, PictureModel& parser, PictureModel& detector);

/// process_picture for AI Eye Contact: `detector` finds the faces of the input picture, each face's rolled crop goes through `landmarker`, and
/// the answer is the landmark table (ai/video/Gaze.h gazeTablePicture: a picture kGazeSlotCount wide, a row per face, the eyelids, irises,
/// nose tip and cheeks of each) instead of a picture of the input's size. `tiles` in the reply is the number of faces looked at.
ai::search::embed::Response processGaze(const ai::search::embed::ProcessPicture& request, PictureModel& landmarker, PictureModel& detector);

} // namespace wabcut::embed_picture
