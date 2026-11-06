#include "PictureModels.h"

#include <wabcut/ai/video/FaceParse.h>
#include <wabcut/ai/video/Gaze.h>
#include <wabcut/ai/video/ModelKeys.h>
#include <wabcut/ai/video/Picture.h>
#include <wabcut/ai/video/Tiling.h>
#include <wabcut/ai/vision/Detection.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <stdexcept>

namespace wabcut::embed_picture {

namespace embed = ai::search::embed;

namespace {

void log(const std::string& text) {
    std::fprintf(stderr, "wabcut-embed: %s\n", text.c_str());
    std::fflush(stderr);
}

std::string pathText(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

/// IEEE 754 half precision (round to nearest even); values beyond its range saturate to infinity.
std::uint16_t toHalf(float value) {
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    const std::uint32_t sign = (bits >> 16) & 0x8000u;
    const std::int32_t exponent = static_cast<std::int32_t>((bits >> 23) & 0xFFu) - 127 + 15;
    std::uint32_t mantissa = bits & 0x7FFFFFu;
    if (((bits >> 23) & 0xFFu) == 0xFFu) { // inf, nan
        return static_cast<std::uint16_t>(sign | 0x7C00u | (mantissa != 0 ? 0x200u : 0u));
    }
    if (exponent >= 31) {
        return static_cast<std::uint16_t>(sign | 0x7C00u);
    }
    if (exponent <= 0) {
        if (exponent < -10) {
            return static_cast<std::uint16_t>(sign);
        }
        mantissa |= 0x800000u;
        const std::uint32_t shift = static_cast<std::uint32_t>(14 - exponent);
        std::uint32_t half = mantissa >> shift;
        const std::uint32_t rest = mantissa & ((1u << shift) - 1u);
        const std::uint32_t middle = 1u << (shift - 1);
        if (rest > middle || (rest == middle && (half & 1u) != 0)) {
            ++half;
        }
        return static_cast<std::uint16_t>(sign | half);
    }
    std::uint32_t half = (static_cast<std::uint32_t>(exponent) << 10) | (mantissa >> 13);
    const std::uint32_t rest = mantissa & 0x1FFFu;
    if (rest > 0x1000u || (rest == 0x1000u && (half & 1u) != 0)) {
        ++half; // may carry into the exponent: correct
    }
    return static_cast<std::uint16_t>(sign | half);
}

float fromHalf(std::uint16_t h) {
    const std::uint32_t sign = (static_cast<std::uint32_t>(h) & 0x8000u) << 16;
    const std::uint32_t exponent = (h >> 10) & 0x1Fu;
    const std::uint32_t mantissa = h & 0x3FFu;
    std::uint32_t bits;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            std::uint32_t e = 127 - 15 + 1;
            std::uint32_t m = mantissa;
            while ((m & 0x400u) == 0) {
                m <<= 1;
                --e;
            }
            bits = sign | (e << 23) | ((m & 0x3FFu) << 13);
        }
    } else if (exponent == 31) {
        bits = sign | 0x7F800000u | (mantissa << 13);
    } else {
        bits = sign | ((exponent - 15 + 127) << 23) | (mantissa << 13);
    }
    return std::bit_cast<float>(bits);
}

} // namespace

PictureKind pictureKindOf(const std::string& key) {
    if (key == ai::video::kKeyInterpolate) {
        return PictureKind::Interpolate;
    }
    if (key == ai::video::kKeyDepth) {
        return PictureKind::Depth;
    }
    if (key == ai::video::kKeyFaceDetect) {
        return PictureKind::FaceDetect;
    }
    if (key == ai::video::kKeyFaceParse) {
        return PictureKind::FaceParse;
    }
    if (key == ai::video::kKeyFaceLandmarks) {
        return PictureKind::FaceLandmarks;
    }
    return PictureKind::Image;
}

PictureModel::PictureModel(std::string key, const std::filesystem::path& file, const std::string& device, int threads,
                           embed_audio::SessionFactory factory)
    : m_kind(pictureKindOf(key)), m_key(std::move(key)), m_file(file), m_threads(threads), m_factory(std::move(factory)) {
    if (!std::filesystem::is_regular_file(file)) {
        throw std::invalid_argument("model file missing: " + pathText(file));
    }
    std::unique_ptr<Ort::Session> session;
    if (device != "cpu") {
        try {
            session = open(true);
        } catch (const std::exception& e) {
            log("DirectML could not load " + pathText(file.filename()) + " (" + e.what() + "); using the CPU");
            session.reset();
        }
    }
    if (!session) {
        session = open(false);
    }
    m_session = std::move(session);
    Ort::AllocatorWithDefaultOptions allocator;
    const std::size_t inputs = m_kind == PictureKind::Interpolate ? 3 : 1;
    if (m_session->GetInputCount() != inputs || m_session->GetOutputCount() < 1) {
        throw std::runtime_error(pathText(file.filename()) + " is not the graph " + m_key + " needs (" + std::to_string(inputs) +
                                 (inputs == 1 ? " input" : " inputs") + " expected)");
    }
    for (std::size_t i = 0; i < inputs; ++i) {
        m_inputNames.push_back(m_session->GetInputNameAllocated(i, allocator).get());
    }
    m_inputName = m_inputNames.front();
    m_outputName = m_session->GetOutputNameAllocated(0, allocator).get();
    for (std::size_t i = 0; i < m_session->GetOutputCount(); ++i) {
        m_outputNames.push_back(m_session->GetOutputNameAllocated(i, allocator).get());
    }
    const auto info = m_session->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo();
    const auto shape = info.GetShape();
    if (shape.size() != 4) {
        throw std::runtime_error(pathText(file.filename()) + " does not take an NCHW picture");
    }
    m_channels = shape[1] == 4 ? 4 : 3;
    const auto type = info.GetElementType();
    if (type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT && type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16) {
        throw std::runtime_error(pathText(file.filename()) + " takes neither float32 nor float16 input");
    }
    m_half = type == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16;
    if (m_kind == PictureKind::FaceDetect && m_outputNames.size() != 12) {
        throw std::runtime_error(pathText(file.filename()) + " is not YuNet 2023mar (twelve outputs expected)");
    }
    if (m_kind == PictureKind::FaceLandmarks && (m_outputNames.size() < 2 || shape[1] != ai::video::kGazeCropSize || shape[2] != ai::video::kGazeCropSize || shape[3] != 3)) {
        throw std::runtime_error(pathText(file.filename()) + " is not the MediaPipe face landmark graph (a 256 x 256 x 3 input and two outputs expected)");
    }
    if (m_kind != PictureKind::Image && m_half) {
        throw std::runtime_error(pathText(file.filename()) + " takes half precision input: only the float32 graphs of " + m_key + " are supported");
    }
}

std::unique_ptr<Ort::Session> PictureModel::open(bool gpu) {
    return m_factory(m_file, gpu, m_threads, m_device);
}

std::expected<void, Error> PictureModel::runOnce(std::span<const float> input, int size, double sigma, int scale, std::vector<float>& output) {
    try {
        const std::size_t plane = static_cast<std::size_t>(size) * static_cast<std::size_t>(size);
        const std::array<std::int64_t, 4> shape{1, m_channels, size, size};
        const std::size_t count = plane * static_cast<std::size_t>(m_channels);
        std::vector<float> in32;
        std::vector<std::uint16_t> in16;
        const char* inputs[] = {m_inputName.c_str()};
        const char* outputs[] = {m_outputName.c_str()};
        const auto value = [&](std::size_t i) { return i < plane * 3 ? input[i] : static_cast<float>(sigma); };
        std::vector<Ort::Value> result;
        if (m_half) {
            in16.resize(count);
            for (std::size_t i = 0; i < count; ++i) {
                in16[i] = toHalf(value(i));
            }
            Ort::Value tensor = Ort::Value::CreateTensor(m_memory, in16.data(), in16.size() * sizeof(std::uint16_t), shape.data(), shape.size(),
                                                         ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16);
            result = m_session->Run(Ort::RunOptions{nullptr}, inputs, &tensor, 1, outputs, 1);
        } else {
            in32.resize(count);
            for (std::size_t i = 0; i < count; ++i) {
                in32[i] = value(i);
            }
            Ort::Value tensor = Ort::Value::CreateTensor<float>(m_memory, in32.data(), in32.size(), shape.data(), shape.size());
            result = m_session->Run(Ort::RunOptions{nullptr}, inputs, &tensor, 1, outputs, 1);
        }
        Ort::Value& out = result.front();
        const auto outShape = out.GetTensorTypeAndShapeInfo().GetShape();
        const std::size_t expected = plane * 3 * static_cast<std::size_t>(scale) * static_cast<std::size_t>(scale);
        if (outShape.size() != 4 || outShape[1] != 3 || outShape[2] != static_cast<std::int64_t>(size) * scale ||
            outShape[3] != static_cast<std::int64_t>(size) * scale) {
            return std::unexpected(Error(ErrorCode::DecoderError, "the model returned a picture of an unexpected shape"));
        }
        output.resize(expected);
        if (out.GetTensorTypeAndShapeInfo().GetElementType() == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16) {
            const std::uint16_t* data = out.GetTensorData<std::uint16_t>();
            for (std::size_t i = 0; i < expected; ++i) {
                output[i] = fromHalf(data[i]);
            }
        } else {
            const float* data = out.GetTensorData<float>();
            std::copy(data, data + expected, output.begin());
        }
        return {};
    } catch (const std::exception& e) {
        return std::unexpected(Error(ErrorCode::DecoderError, e.what()));
    }
}

std::expected<void, Error> PictureModel::run(std::span<const float> input, int size, double sigma, int scale, std::vector<float>& output) {
    auto first = runOnce(input, size, sigma, scale, output);
    if (first || m_device == "CPU") {
        return first;
    }
    // The GPU failed while running (a driver reset, out of video memory): the graph again on the CPU, and the tile with it.
    log("DirectML failed while running " + pathText(m_file.filename()) + " (" + first.error().message + "); switching to the CPU");
    try {
        m_session = open(false);
    } catch (const std::exception& e) {
        return std::unexpected(Error(ErrorCode::DecoderError, std::string("the model failed on the GPU and the CPU: ") + e.what()));
    }
    return runOnce(input, size, sigma, scale, output);
}

template <class F>
std::expected<void, Error> PictureModel::withCpuFallback(F&& once) {
    auto first = once();
    if (first || m_device == "CPU") {
        return first;
    }
    // The GPU failed while running (a driver reset, out of video memory, an operator DirectML lacks): the graph on the CPU.
    log("DirectML failed while running " + pathText(m_file.filename()) + " (" + first.error().message + "); switching to the CPU");
    try {
        m_session = open(false);
    } catch (const std::exception& e) {
        return std::unexpected(Error(ErrorCode::DecoderError, std::string("the model failed on the GPU and the CPU: ") + e.what()));
    }
    return once();
}

std::expected<void, Error> PictureModel::interpolateOnce(std::span<const float> a, std::span<const float> b, int width, int height,
                                                         float timestep, std::vector<float>& out) {
    try {
        const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3;
        if (a.size() != count || b.size() != count) {
            return std::unexpected(Error(ErrorCode::InvalidArgument, "the pictures do not match the size"));
        }
        const std::array<std::int64_t, 4> shape{1, 3, height, width};
        const std::array<std::int64_t, 4> stepShape{1, 1, 1, 1};
        float step = timestep;
        std::vector<Ort::Value> inputs;
        inputs.push_back(Ort::Value::CreateTensor<float>(m_memory, const_cast<float*>(a.data()), a.size(), shape.data(), shape.size()));
        inputs.push_back(Ort::Value::CreateTensor<float>(m_memory, const_cast<float*>(b.data()), b.size(), shape.data(), shape.size()));
        inputs.push_back(Ort::Value::CreateTensor<float>(m_memory, &step, 1, stepShape.data(), stepShape.size()));
        const char* names[] = {m_inputNames[0].c_str(), m_inputNames[1].c_str(), m_inputNames[2].c_str()};
        const char* outputs[] = {m_outputName.c_str()};
        auto result = m_session->Run(Ort::RunOptions{nullptr}, names, inputs.data(), inputs.size(), outputs, 1);
        const auto outShape = result.front().GetTensorTypeAndShapeInfo().GetShape();
        if (outShape.size() != 4 || outShape[1] != 3 || outShape[2] != height || outShape[3] != width) {
            return std::unexpected(Error(ErrorCode::DecoderError, "the model returned a picture of an unexpected shape"));
        }
        const float* data = result.front().GetTensorData<float>();
        out.assign(data, data + count);
        return {};
    } catch (const std::exception& e) {
        return std::unexpected(Error(ErrorCode::DecoderError, e.what()));
    }
}

std::expected<void, Error> PictureModel::interpolate(std::span<const float> a, std::span<const float> b, int width, int height,
                                                     float timestep, std::vector<float>& out) {
    if (m_kind != PictureKind::Interpolate) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, m_key + " is not a frame interpolation model"));
    }
    return withCpuFallback([&] { return interpolateOnce(a, b, width, height, timestep, out); });
}

std::expected<void, Error> PictureModel::depthOnce(std::span<const float> input, int width, int height, std::vector<float>& out) {
    try {
        const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3;
        if (input.size() != count) {
            return std::unexpected(Error(ErrorCode::InvalidArgument, "the picture does not match the size"));
        }
        const std::array<std::int64_t, 4> shape{1, 3, height, width};
        Ort::Value tensor = Ort::Value::CreateTensor<float>(m_memory, const_cast<float*>(input.data()), input.size(), shape.data(), shape.size());
        const char* inputs[] = {m_inputName.c_str()};
        const char* outputs[] = {m_outputName.c_str()};
        auto result = m_session->Run(Ort::RunOptions{nullptr}, inputs, &tensor, 1, outputs, 1);
        const auto outShape = result.front().GetTensorTypeAndShapeInfo().GetShape();
        // predicted_depth is [batch, 14 * floor(height / 14), 14 * floor(width / 14)]: our sides are multiples of 14.
        if (outShape.size() != 3 || outShape[0] != 1 || outShape[1] != height || outShape[2] != width) {
            return std::unexpected(Error(ErrorCode::DecoderError, "the model returned a depth map of an unexpected shape"));
        }
        const float* data = result.front().GetTensorData<float>();
        out.assign(data, data + static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
        return {};
    } catch (const std::exception& e) {
        return std::unexpected(Error(ErrorCode::DecoderError, e.what()));
    }
}

std::expected<void, Error> PictureModel::depth(std::span<const float> input, int width, int height, std::vector<float>& out) {
    if (m_kind != PictureKind::Depth) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, m_key + " is not a depth model"));
    }
    return withCpuFallback([&] { return depthOnce(input, width, height, out); });
}

std::expected<void, Error> PictureModel::detectOnce(std::span<const float> input, std::vector<std::pair<std::string, std::vector<float>>>& outputs) {
    try {
        const int size = ai::vision::kYuNetSize;
        if (input.size() != static_cast<std::size_t>(size) * static_cast<std::size_t>(size) * 3) {
            return std::unexpected(Error(ErrorCode::InvalidArgument, "the picture does not match the detector's input"));
        }
        const std::array<std::int64_t, 4> shape{1, 3, size, size};
        Ort::Value tensor = Ort::Value::CreateTensor<float>(m_memory, const_cast<float*>(input.data()), input.size(), shape.data(), shape.size());
        const char* inputs[] = {m_inputName.c_str()};
        std::vector<const char*> names;
        for (const std::string& name : m_outputNames) {
            names.push_back(name.c_str());
        }
        auto result = m_session->Run(Ort::RunOptions{nullptr}, inputs, &tensor, 1, names.data(), names.size());
        outputs.clear();
        for (std::size_t i = 0; i < result.size(); ++i) {
            const std::size_t count = result[i].GetTensorTypeAndShapeInfo().GetElementCount();
            const float* data = result[i].GetTensorData<float>();
            outputs.emplace_back(m_outputNames[i], std::vector<float>(data, data + count));
        }
        return {};
    } catch (const std::exception& e) {
        return std::unexpected(Error(ErrorCode::DecoderError, e.what()));
    }
}

std::expected<void, Error> PictureModel::detectFaces(std::span<const float> input, std::vector<std::pair<std::string, std::vector<float>>>& outputs) {
    if (m_kind != PictureKind::FaceDetect) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, m_key + " is not a face detector"));
    }
    return withCpuFallback([&] { return detectOnce(input, outputs); });
}

std::expected<void, Error> PictureModel::parseOnce(std::span<const float> input, std::vector<float>& logits) {
    try {
        const int size = ai::video::kFaceParseSize;
        const std::size_t plane = static_cast<std::size_t>(size) * static_cast<std::size_t>(size);
        if (input.size() != plane * 3) {
            return std::unexpected(Error(ErrorCode::InvalidArgument, "the crop does not match the parser's input"));
        }
        const std::array<std::int64_t, 4> shape{1, 3, size, size};
        Ort::Value tensor = Ort::Value::CreateTensor<float>(m_memory, const_cast<float*>(input.data()), input.size(), shape.data(), shape.size());
        const char* inputs[] = {m_inputName.c_str()};
        const char* outputs[] = {m_outputName.c_str()}; // the main head; the two auxiliary heads are not asked for
        auto result = m_session->Run(Ort::RunOptions{nullptr}, inputs, &tensor, 1, outputs, 1);
        const auto outShape = result.front().GetTensorTypeAndShapeInfo().GetShape();
        if (outShape.size() != 4 || outShape[0] != 1 || outShape[1] != 19 || outShape[2] != size || outShape[3] != size) {
            return std::unexpected(Error(ErrorCode::DecoderError, "the face parser returned logits of an unexpected shape"));
        }
        const float* data = result.front().GetTensorData<float>();
        logits.assign(data, data + plane * 19);
        return {};
    } catch (const std::exception& e) {
        return std::unexpected(Error(ErrorCode::DecoderError, e.what()));
    }
}

std::expected<void, Error> PictureModel::parseFace(std::span<const float> input, std::vector<float>& logits) {
    if (m_kind != PictureKind::FaceParse) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, m_key + " is not a face parser"));
    }
    return withCpuFallback([&] { return parseOnce(input, logits); });
}

std::expected<void, Error> PictureModel::landmarkOnce(std::span<const float> input, std::vector<float>& landmarks, float& presenceLogit) {
    try {
        const int size = ai::video::kGazeCropSize;
        if (input.size() != static_cast<std::size_t>(size) * static_cast<std::size_t>(size) * 3) {
            return std::unexpected(Error(ErrorCode::InvalidArgument, "the crop does not match the landmark network's input"));
        }
        const std::array<std::int64_t, 4> shape{1, size, size, 3};
        Ort::Value tensor = Ort::Value::CreateTensor<float>(m_memory, const_cast<float*>(input.data()), input.size(), shape.data(), shape.size());
        const char* inputs[] = {m_inputName.c_str()};
        const char* outputs[] = {m_outputNames[0].c_str(), m_outputNames[1].c_str()}; // the landmarks and the presence logit
        auto result = m_session->Run(Ort::RunOptions{nullptr}, inputs, &tensor, 1, outputs, 2);
        if (result.size() != 2 || result[0].GetTensorTypeAndShapeInfo().GetElementCount() != static_cast<std::size_t>(ai::video::kGazeLandmarkCount) * 3 ||
            result[1].GetTensorTypeAndShapeInfo().GetElementCount() != 1) {
            return std::unexpected(Error(ErrorCode::DecoderError, "the landmark network returned outputs of an unexpected shape"));
        }
        const float* data = result[0].GetTensorData<float>();
        landmarks.assign(data, data + static_cast<std::size_t>(ai::video::kGazeLandmarkCount) * 3);
        presenceLogit = result[1].GetTensorData<float>()[0];
        return {};
    } catch (const std::exception& e) {
        return std::unexpected(Error(ErrorCode::DecoderError, e.what()));
    }
}

std::expected<void, Error> PictureModel::landmarkFace(std::span<const float> input, std::vector<float>& landmarks, float& presenceLogit) {
    if (m_kind != PictureKind::FaceLandmarks) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, m_key + " is not a face landmark model"));
    }
    return withCpuFallback([&] { return landmarkOnce(input, landmarks, presenceLogit); });
}

namespace {

/// A picture scaled to fit `maxSide` on its longer side (never larger), both sides even.
ai::video::Picture fitted(const ai::video::Picture& in, int maxSide) {
    const int longest = std::max(in.width, in.height);
    if (longest <= maxSide) {
        return in;
    }
    const double k = static_cast<double>(maxSide) / longest;
    const int w = std::max(2, static_cast<int>(std::lround(in.width * k)) / 2 * 2);
    const int h = std::max(2, static_cast<int>(std::lround(in.height * k)) / 2 * 2);
    return ai::video::resized(in, w, h);
}

/// Planar float RGB (0..1) of `p`, padded to width x height by repeating the last column and row.
std::vector<float> planarPadded(const ai::video::Picture& p, int width, int height) {
    std::vector<float> out(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3);
    const std::size_t plane = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    for (int y = 0; y < height; ++y) {
        const int sy = std::min(y, p.height - 1);
        for (int x = 0; x < width; ++x) {
            const int sx = std::min(x, p.width - 1);
            const std::size_t src = (static_cast<std::size_t>(sy) * static_cast<std::size_t>(p.width) + static_cast<std::size_t>(sx)) * 3;
            const std::size_t dst = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
            for (std::size_t c = 0; c < 3; ++c) {
                out[c * plane + dst] = static_cast<float>(p.rgb[src + c]) / 65535.0f;
            }
        }
    }
    return out;
}

embed::Response processInterpolation(const embed::ProcessPicture& r, PictureModel& model) {
    namespace video = ai::video;
    auto first = embed::readPictureFile(r.input);
    auto second = embed::readPictureFile(r.input2);
    if (!first) {
        return embed::Failure{r.id, "io", first.error().message};
    }
    if (!second) {
        return embed::Failure{r.id, "io", second.error().message};
    }
    const auto started = std::chrono::steady_clock::now();
    video::Picture a;
    a.width = r.input.width;
    a.height = r.input.height;
    a.rgb = std::move(*first);
    video::Picture b = a;
    b.rgb = std::move(*second);
    // Larger pictures are interpolated at 1920 on the long side (RIFE's memory grows with the area) and the result is scaled up.
    const video::Picture wa = fitted(a, video::kInterpolateMaxSide);
    const video::Picture wb = wa.width == a.width ? b : video::resized(b, wa.width, wa.height);
    const int padW = (wa.width + video::kInterpolateMultiple - 1) / video::kInterpolateMultiple * video::kInterpolateMultiple;
    const int padH = (wa.height + video::kInterpolateMultiple - 1) / video::kInterpolateMultiple * video::kInterpolateMultiple;
    const std::vector<float> fa = planarPadded(wa, padW, padH);
    const std::vector<float> fb = planarPadded(wb, padW, padH);
    std::vector<float> merged;
    if (auto ran = model.interpolate(fa, fb, padW, padH, static_cast<float>(r.timestep), merged); !ran) {
        return embed::Failure{r.id, "inference", ran.error().message};
    }
    video::Picture result;
    result.width = wa.width;
    result.height = wa.height;
    result.rgb.resize(static_cast<std::size_t>(wa.width) * static_cast<std::size_t>(wa.height) * 3);
    const std::size_t plane = static_cast<std::size_t>(padW) * static_cast<std::size_t>(padH);
    for (int y = 0; y < wa.height; ++y) {
        for (int x = 0; x < wa.width; ++x) {
            const std::size_t src = static_cast<std::size_t>(y) * static_cast<std::size_t>(padW) + static_cast<std::size_t>(x);
            const std::size_t dst = (static_cast<std::size_t>(y) * static_cast<std::size_t>(wa.width) + static_cast<std::size_t>(x)) * 3;
            for (std::size_t c = 0; c < 3; ++c) {
                const float v = merged[c * plane + src];
                result.rgb[dst + c] = static_cast<std::uint16_t>(std::lround(std::clamp(std::isfinite(v) ? v : 0.0f, 0.0f, 1.0f) * 65535.0f));
            }
        }
    }
    if (result.width != a.width || result.height != a.height) {
        result = video::resized(result, a.width, a.height);
    }
    auto written = embed::writePictureFile(r.output, result.width, result.height, result.rgb);
    if (!written) {
        return embed::Failure{r.id, "io", written.error().message};
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    return embed::PictureDone{r.id, result.width, result.height, 1, ms, model.device()};
}

embed::Response processDepth(const embed::ProcessPicture& r, PictureModel& model) {
    namespace video = ai::video;
    auto rgb = embed::readPictureFile(r.input);
    if (!rgb) {
        return embed::Failure{r.id, "io", rgb.error().message};
    }
    const auto started = std::chrono::steady_clock::now();
    video::Picture picture;
    picture.width = r.input.width;
    picture.height = r.input.height;
    picture.rgb = std::move(*rgb);
    // The model's patches are 14 pixels: look at the picture at the nearest multiples (the answer is scaled back).
    const int w = std::max(video::kDepthMultiple, (picture.width + 7) / video::kDepthMultiple * video::kDepthMultiple);
    const int h = std::max(video::kDepthMultiple, (picture.height + 7) / video::kDepthMultiple * video::kDepthMultiple);
    const video::Picture seen = (w == picture.width && h == picture.height) ? picture : video::resized(picture, w, h);
    constexpr std::array<float, 3> kMean{0.485f, 0.456f, 0.406f};
    constexpr std::array<float, 3> kDeviation{0.229f, 0.224f, 0.225f};
    const std::size_t plane = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    std::vector<float> input(plane * 3);
    for (std::size_t i = 0; i < plane; ++i) {
        for (std::size_t c = 0; c < 3; ++c) {
            input[c * plane + i] = (static_cast<float>(seen.rgb[3 * i + c]) / 65535.0f - kMean[c]) / kDeviation[c];
        }
    }
    std::vector<float> depth;
    if (auto ran = model.depth(input, w, h, depth); !ran) {
        return embed::Failure{r.id, "inference", ran.error().message};
    }
    video::Picture grey = video::greyPicture(video::normalisedDepth(depth), w, h);
    if (grey.width != picture.width || grey.height != picture.height) {
        grey = video::resized(grey, picture.width, picture.height);
    }
    auto written = embed::writePictureFile(r.output, grey.width, grey.height, grey.rgb);
    if (!written) {
        return embed::Failure{r.id, "io", written.error().message};
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    return embed::PictureDone{r.id, grey.width, grey.height, 1, ms, model.device()};
}

} // namespace

namespace {

/// The faces YuNet finds in `picture` (at most as it decodes them: score 0.6, NMS 0.3), in the picture's pixels, best first.
std::expected<std::vector<ai::vision::Detection>, Error> detectedFaces(const ai::video::Picture& picture, PictureModel& detector) {
    namespace vision = ai::vision;
    // YuNet takes 8-bit pictures.
    ai::search::Image eight;
    eight.width = picture.width;
    eight.height = picture.height;
    eight.rgb.resize(picture.rgb.size());
    for (std::size_t i = 0; i < picture.rgb.size(); ++i) {
        eight.rgb[i] = static_cast<std::uint8_t>((static_cast<unsigned>(picture.rgb[i]) + 128u) / 257u);
    }
    vision::Letterbox box;
    const std::vector<float> detectorInput = vision::yunetInput(eight, box);
    std::vector<std::pair<std::string, std::vector<float>>> outputs;
    if (auto ran = detector.detectFaces(detectorInput, outputs); !ran) {
        return std::unexpected(ran.error());
    }
    const auto named = [&](const std::string& name) -> std::span<const float> {
        for (const auto& [key, values] : outputs) {
            if (key == name) {
                return values;
            }
        }
        return {};
    };
    vision::YuNetOutputs yunet;
    constexpr const char* kStrides[3] = {"8", "16", "32"};
    for (std::size_t s = 0; s < 3; ++s) {
        yunet.cls[s] = named(std::string("cls_") + kStrides[s]);
        yunet.obj[s] = named(std::string("obj_") + kStrides[s]);
        yunet.bbox[s] = named(std::string("bbox_") + kStrides[s]);
        yunet.kps[s] = named(std::string("kps_") + kStrides[s]);
    }
    return vision::decodeYuNet(yunet, box, 0.6f, 0.3f);
}

} // namespace

embed::Response processFaces(const embed::ProcessPicture& r, PictureModel& parser, PictureModel& detector) {
    namespace video = ai::video;
    namespace vision = ai::vision;
    if (parser.kind() != PictureKind::FaceParse || detector.kind() != PictureKind::FaceDetect) {
        return embed::Failure{r.id, "bad_request", "the face map needs the face parser and the face detector"};
    }
    auto rgb = embed::readPictureFile(r.input);
    if (!rgb) {
        return embed::Failure{r.id, "io", rgb.error().message};
    }
    const auto started = std::chrono::steady_clock::now();
    video::Picture picture;
    picture.width = r.input.width;
    picture.height = r.input.height;
    picture.rgb = std::move(*rgb);
    auto found = detectedFaces(picture, detector);
    if (!found) {
        return embed::Failure{r.id, "inference", found.error().message};
    }
    const std::vector<vision::Detection>& faces = *found;
    video::FaceMapBytes map(picture.width, picture.height);
    int looked = 0;
    for (const vision::Detection& face : faces) {
        if (looked >= video::kFaceMaxFaces) {
            break;
        }
        if (std::max(face.box.w, face.box.h) < 24.0f) {
            continue; // too small to tell its parts apart
        }
        const video::FaceCrop crop = video::faceCropFor({face.box.x, face.box.y, face.box.w, face.box.h});
        const std::vector<float> input = video::faceParseInput(picture, crop);
        std::vector<float> logits;
        if (auto ran = parser.parseFace(input, logits); !ran) {
            return embed::Failure{r.id, "inference", ran.error().message};
        }
        const video::FaceClasses classes = video::faceClassesOf(logits);
        video::pasteFace(map, classes, crop);
        ++looked;
    }
    const video::Picture result = video::faceMapPicture(map);
    auto written = embed::writePictureFile(r.output, result.width, result.height, result.rgb);
    if (!written) {
        return embed::Failure{r.id, "io", written.error().message};
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    return embed::PictureDone{r.id, result.width, result.height, looked, ms, parser.device()};
}

embed::Response processGaze(const embed::ProcessPicture& r, PictureModel& landmarker, PictureModel& detector) {
    namespace video = ai::video;
    namespace vision = ai::vision;
    if (landmarker.kind() != PictureKind::FaceLandmarks || detector.kind() != PictureKind::FaceDetect) {
        return embed::Failure{r.id, "bad_request", "the eye measurements need the face landmark model and the face detector"};
    }
    auto rgb = embed::readPictureFile(r.input);
    if (!rgb) {
        return embed::Failure{r.id, "io", rgb.error().message};
    }
    const auto started = std::chrono::steady_clock::now();
    video::Picture picture;
    picture.width = r.input.width;
    picture.height = r.input.height;
    picture.rgb = std::move(*rgb);
    auto found = detectedFaces(picture, detector);
    if (!found) {
        return embed::Failure{r.id, "inference", found.error().message};
    }
    std::vector<video::GazeLandmarkFace> measured;
    const std::span<const int> indices = video::gazeLandmarkIndices();
    for (const vision::Detection& face : *found) {
        if (static_cast<int>(measured.size()) >= kGazeMaxFaces) {
            break;
        }
        if (std::max(face.box.w, face.box.h) < 24.0f || face.landmarks.size() < 4) {
            continue; // too small to find an iris in
        }
        const video::FaceBox box{face.box.x, face.box.y, face.box.w, face.box.h};
        const video::GazeCrop crop = video::gazeCropFor(box, {face.landmarks[0], face.landmarks[1]}, {face.landmarks[2], face.landmarks[3]});
        const std::vector<float> input = video::gazeInput(picture, crop);
        std::vector<float> output;
        float presenceLogit = 0.0f;
        if (auto ran = landmarker.landmarkFace(input, output, presenceLogit); !ran) {
            return embed::Failure{r.id, "inference", ran.error().message};
        }
        const std::vector<video::LandmarkPoint> all = video::gazeLandmarksOf(output, crop);
        const double presence = 1.0 / (1.0 + std::exp(-static_cast<double>(presenceLogit)));
        if (all.empty() || presence < 0.5) {
            continue; // the network does not see a face in this crop: no measurement is better than a wrong one
        }
        video::GazeLandmarkFace measure;
        measure.box = box;
        measure.presence = presence;
        for (const int index : indices) {
            measure.points.push_back(all[static_cast<std::size_t>(index)]);
        }
        measured.push_back(std::move(measure));
    }
    const video::Picture table = video::gazeTablePicture(measured, picture.width, picture.height);
    auto written = embed::writePictureFile(r.output, table.width, table.height, table.rgb);
    if (!written) {
        return embed::Failure{r.id, "io", written.error().message};
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    return embed::PictureDone{r.id, table.width, table.height, static_cast<int>(measured.size()), ms, landmarker.device()};
}

embed::Response process(const embed::ProcessPicture& r, PictureModel& model) {
    if (model.kind() == PictureKind::FaceDetect || model.kind() == PictureKind::FaceParse || model.kind() == PictureKind::FaceLandmarks) {
        return embed::Failure{r.id, "bad_request", "the face models work together: use processFaces or processGaze"};
    }
    if (model.kind() == PictureKind::Interpolate) {
        return processInterpolation(r, model);
    }
    if (model.kind() == PictureKind::Depth) {
        return processDepth(r, model);
    }
    auto rgb = embed::readPictureFile(r.input);
    if (!rgb) {
        return embed::Failure{r.id, "io", rgb.error().message};
    }
    ai::video::Picture picture;
    picture.width = r.input.width;
    picture.height = r.input.height;
    picture.rgb = std::move(*rgb);
    const ai::video::TileSpec spec{r.tile, r.pad, r.scale};
    const auto started = std::chrono::steady_clock::now();
    auto tiled = ai::video::runTiled(picture, spec, [&](std::span<const float> input, int size, std::vector<float>& output) {
        return model.run(input, size, r.sigma, r.scale, output);
    });
    if (!tiled) {
        return embed::Failure{r.id, "inference", tiled.error().message};
    }
    auto written = embed::writePictureFile(r.output, tiled->picture.width, tiled->picture.height, tiled->picture.rgb);
    if (!written) {
        return embed::Failure{r.id, "io", written.error().message};
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    return embed::PictureDone{r.id, tiled->picture.width, tiled->picture.height, tiled->tiles, ms, model.device()};
}

} // namespace wabcut::embed_picture
