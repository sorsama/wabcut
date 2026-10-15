#include "AudioModels.h"

#include <wabcut/model/MediaLocator.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace wabcut::embed_audio {

namespace embed = ai::search::embed;
namespace dfn = ai::audio::dfn;

namespace {

void log(const std::string& text) {
    std::fprintf(stderr, "wabcut-embed: %s\n", text.c_str());
    std::fflush(stderr);
}

std::string pathText(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::string shapeText(const std::vector<std::int64_t>& shape) {
    std::string out = "[";
    for (std::size_t i = 0; i < shape.size(); ++i) {
        out += (i ? "," : "") + std::to_string(shape[i]);
    }
    return out + "]";
}

Graph openGraph(const std::filesystem::path& file, bool gpu, int threads, const SessionFactory& factory, std::string& device) {
    if (!std::filesystem::is_regular_file(file)) {
        throw std::invalid_argument("model file missing: " + pathText(file));
    }
    Graph g;
    g.file = file;
    g.session = factory(file, gpu, threads, device);
    Ort::AllocatorWithDefaultOptions allocator;
    std::string described = pathText(file.filename()) + " on " + device + ":";
    for (std::size_t i = 0; i < g.session->GetInputCount(); ++i) {
        g.inputs.emplace_back(g.session->GetInputNameAllocated(i, allocator).get());
        described += " in " + g.inputs.back() + shapeText(g.session->GetInputTypeInfo(i).GetTensorTypeAndShapeInfo().GetShape());
    }
    for (std::size_t i = 0; i < g.session->GetOutputCount(); ++i) {
        g.outputs.emplace_back(g.session->GetOutputNameAllocated(i, allocator).get());
        described += " out " + g.outputs.back() + shapeText(g.session->GetOutputTypeInfo(i).GetTensorTypeAndShapeInfo().GetShape());
    }
    log(described);
    return g;
}

/// The graph's names in the order `wanted` lists them (by name; positionally when the export named them otherwise).
std::vector<const char*> ordered(const std::vector<std::string>& names, std::initializer_list<const char*> wanted, const char* what) {
    if (names.size() != wanted.size()) {
        throw std::runtime_error(std::string(what) + " has " + std::to_string(names.size()) + " tensors, not " +
                                 std::to_string(wanted.size()) + " (not DeepFilterNet3?)");
    }
    std::vector<const char*> out;
    bool byName = true;
    for (const char* w : wanted) {
        const auto it = std::ranges::find(names, std::string(w));
        if (it == names.end()) {
            byName = false;
            break;
        }
        out.push_back(it->c_str());
    }
    if (!byName) {
        out.clear();
        for (const std::string& n : names) {
            out.push_back(n.c_str());
        }
    }
    return out;
}

/// The output named `name`, else the graph's first (df_dec.onnx has a second, unnamed one libDF does not read).
const char* outputNamed(const Graph& g, const char* name) {
    const auto it = std::ranges::find(g.outputs, std::string(name));
    return it != g.outputs.end() ? it->c_str() : g.outputs.front().c_str();
}

Ort::MemoryInfo& cpuMemory() {
    static Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    return memory;
}

Ort::Value view(float* data, std::size_t count, const std::vector<std::int64_t>& shape) {
    return Ort::Value::CreateTensor<float>(cpuMemory(), data, count, shape.data(), shape.size());
}

std::size_t elements(const Ort::Value& v) {
    return v.GetTensorTypeAndShapeInfo().GetElementCount();
}

/// Writes `samples` to `path`; throws on failure.
void writeFloats(const std::filesystem::path& path, const std::vector<float>& samples) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(samples.data()), static_cast<std::streamsize>(samples.size() * sizeof(float)));
    out.close();
    if (!out) {
        throw std::ios_base::failure("cannot write " + pathText(path));
    }
}

} // namespace

// --- DeepFilterNet3 -----------------------------------------------------------------------------------------------

SpeechModel::SpeechModel(const std::filesystem::path& directory, const std::string& device, int threads, SessionFactory factory)
    : m_directory(directory), m_threads(threads), m_factory(std::move(factory)) {
    bool opened = false;
#ifdef _WIN32
    if (device != "cpu") {
        try {
            open(true);
            opened = true;
        } catch (const std::invalid_argument&) {
            throw;
        } catch (const std::exception& e) {
            log(std::string("DirectML could not load DeepFilterNet3 (") + e.what() + "); using the CPU");
        }
    }
#else
    (void)device;
#endif
    if (!opened) {
        open(false);
    }
}

void SpeechModel::open(bool gpu) {
    std::string device;
    m_enc = openGraph(m_directory / "enc.onnx", gpu, m_threads, m_factory, device);
    m_erbDec = openGraph(m_directory / "erb_dec.onnx", gpu, m_threads, m_factory, device);
    m_dfDec = openGraph(m_directory / "df_dec.onnx", gpu, m_threads, m_factory, device);
    m_device = device;
    // The graphs' contract (libDF tract.rs): 2 inputs / 7 outputs, 5 / 1, 2 / 1.
    (void)ordered(m_enc.inputs, {"feat_erb", "feat_spec"}, "enc.onnx");
    (void)ordered(m_enc.outputs, {"e0", "e1", "e2", "e3", "emb", "c0", "lsnr"}, "enc.onnx");
    (void)ordered(m_erbDec.inputs, {"emb", "e3", "e2", "e1", "e0"}, "erb_dec.onnx");
    (void)ordered(m_dfDec.inputs, {"emb", "c0"}, "df_dec.onnx");
    if (m_erbDec.outputs.empty() || m_dfDec.outputs.empty()) {
        throw std::runtime_error("the DeepFilterNet3 decoders have no outputs");
    }
}

dfn::NetworkOutput SpeechModel::run(const dfn::Features& features) {
    try {
        return runOnce(features);
    } catch (const std::exception& e) {
        if (m_device == "CPU") {
            throw;
        }
        log(std::string("DirectML failed while running DeepFilterNet3 (") + e.what() + "); switching to the CPU");
        open(false);
        return runOnce(features);
    }
}

dfn::NetworkOutput SpeechModel::runOnce(const dfn::Features& features) {
    const std::int64_t t = features.frames;
    std::vector<float> erb = features.erb;
    std::vector<float> spec = features.spec;
    std::array<Ort::Value, 2> encIn{view(erb.data(), erb.size(), {1, 1, t, dfn::kErbBands}),
                                   view(spec.data(), spec.size(), {1, 2, t, dfn::kDfBins})};
    const auto encInNames = ordered(m_enc.inputs, {"feat_erb", "feat_spec"}, "enc.onnx");
    const auto encOutNames = ordered(m_enc.outputs, {"e0", "e1", "e2", "e3", "emb", "c0", "lsnr"}, "enc.onnx");
    auto enc = m_enc.session->Run(Ort::RunOptions{nullptr}, encInNames.data(), encIn.data(), encIn.size(), encOutNames.data(),
                                  encOutNames.size());
    // e0 e1 e2 e3 emb c0 lsnr, viewed (not copied) as the decoders' inputs.
    const auto again = [&](std::size_t i) {
        Ort::Value& v = enc[i];
        return view(v.GetTensorMutableData<float>(), elements(v), v.GetTensorTypeAndShapeInfo().GetShape());
    };
    std::array<Ort::Value, 5> erbIn{again(4), again(3), again(2), again(1), again(0)};
    const auto erbInNames = ordered(m_erbDec.inputs, {"emb", "e3", "e2", "e1", "e0"}, "erb_dec.onnx");
    const char* erbOut[] = {outputNamed(m_erbDec, "m")};
    auto mask = m_erbDec.session->Run(Ort::RunOptions{nullptr}, erbInNames.data(), erbIn.data(), erbIn.size(), erbOut, 1);
    std::array<Ort::Value, 2> dfIn{again(4), again(5)};
    const auto dfInNames = ordered(m_dfDec.inputs, {"emb", "c0"}, "df_dec.onnx");
    const char* dfOut[] = {outputNamed(m_dfDec, "coefs")};
    auto coefs = m_dfDec.session->Run(Ort::RunOptions{nullptr}, dfInNames.data(), dfIn.data(), dfIn.size(), dfOut, 1);

    dfn::NetworkOutput out;
    out.frames = features.frames;
    const auto frames = static_cast<std::size_t>(t);
    if (elements(mask.front()) != frames * dfn::kErbBands || elements(coefs.front()) != frames * dfn::kDfBins * dfn::kDfOrder * 2 ||
        elements(enc[6]) != frames) {
        throw std::runtime_error("DeepFilterNet3 returned tensors of unexpected sizes (mask " + std::to_string(elements(mask.front())) +
                                 ", coefs " + std::to_string(elements(coefs.front())) + ", lsnr " + std::to_string(elements(enc[6])) +
                                 " for " + std::to_string(frames) + " frames)");
    }
    const float* m = mask.front().GetTensorData<float>();
    out.gains.assign(m, m + frames * dfn::kErbBands);
    const float* c = coefs.front().GetTensorData<float>();
    out.coefs.assign(c, c + frames * dfn::kDfBins * dfn::kDfOrder * 2);
    const float* l = enc[6].GetTensorData<float>();
    out.lsnr.assign(l, l + frames);
    return out;
}

// --- HTDemucs ------------------------------------------------------------------------------------------------------

StemModel::StemModel(const std::filesystem::path& file, const std::string& device, int threads, SessionFactory factory)
    : m_threads(threads), m_factory(std::move(factory)) {
    m_graph.file = file;
    bool opened = false;
#ifdef _WIN32
    if (device != "cpu") {
        try {
            open(true);
            opened = true;
        } catch (const std::invalid_argument&) {
            throw;
        } catch (const std::exception& e) {
            log(std::string("DirectML could not load the stem model (") + e.what() + "); using the CPU");
        }
    }
#else
    (void)device;
#endif
    if (!opened) {
        open(false);
    }
}

void StemModel::open(bool gpu) {
    const std::filesystem::path file = m_graph.file;
    m_graph = openGraph(file, gpu, m_threads, m_factory, m_device);
    if (m_graph.inputs.size() != 1 || m_graph.outputs.empty()) {
        throw std::runtime_error("the stem model must have one input (the mix)");
    }
    const auto in = m_graph.session->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
    const auto out = m_graph.session->GetOutputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
    if (in.size() != 3 || in[1] != 2 || in[2] <= 0 || out.size() != 4 || out[1] <= 0 || out[2] != 2) {
        throw std::runtime_error("the stem model is not [1, 2, segment] -> [1, stems, 2, segment] (got " + shapeText(in) + " -> " +
                                 shapeText(out) + ")");
    }
    m_segment = static_cast<int>(in[2]);
    m_stems = static_cast<int>(out[1]);
}

std::vector<float> StemModel::run(const std::vector<float>& interleaved, int frames) {
    if (frames <= 0 || frames > m_segment || interleaved.size() != static_cast<std::size_t>(frames) * 2) {
        throw std::invalid_argument("a stem segment must have 1 to " + std::to_string(m_segment) + " stereo frames");
    }
    // [1, 2, segment] planar, zero padded.
    std::vector<float> planar(static_cast<std::size_t>(m_segment) * 2, 0.0f);
    for (int f = 0; f < frames; ++f) {
        planar[static_cast<std::size_t>(f)] = interleaved[static_cast<std::size_t>(f) * 2];
        planar[static_cast<std::size_t>(m_segment + f)] = interleaved[static_cast<std::size_t>(f) * 2 + 1];
    }
    std::vector<float> stems;
    try {
        stems = runOnce(planar);
    } catch (const std::exception& e) {
        if (m_device == "CPU") {
            throw;
        }
        log(std::string("DirectML failed while running the stem model (") + e.what() + "); switching to the CPU");
        open(false);
        stems = runOnce(planar);
    }
    // [stems, 2, segment] planar -> stems blocks of frames x 2 interleaved.
    std::vector<float> out(static_cast<std::size_t>(m_stems) * static_cast<std::size_t>(frames) * 2);
    for (int s = 0; s < m_stems; ++s) {
        const float* left = stems.data() + (static_cast<std::size_t>(s) * 2) * static_cast<std::size_t>(m_segment);
        const float* right = left + m_segment;
        float* dst = out.data() + static_cast<std::size_t>(s) * static_cast<std::size_t>(frames) * 2;
        for (int f = 0; f < frames; ++f) {
            dst[static_cast<std::size_t>(f) * 2] = left[f];
            dst[static_cast<std::size_t>(f) * 2 + 1] = right[f];
        }
    }
    return out;
}

std::vector<float> StemModel::runOnce(const std::vector<float>& planar) {
    std::vector<float> input = planar;
    Ort::Value tensor = view(input.data(), input.size(), {1, 2, m_segment});
    const char* inputs[] = {m_graph.inputs.front().c_str()};
    const char* outputs[] = {m_graph.outputs.front().c_str()};
    auto result = m_graph.session->Run(Ort::RunOptions{nullptr}, inputs, &tensor, 1, outputs, 1);
    const std::size_t count = static_cast<std::size_t>(m_stems) * 2 * static_cast<std::size_t>(m_segment);
    if (elements(result.front()) != count) {
        throw std::runtime_error("the stem model returned " + std::to_string(elements(result.front())) + " samples, not " +
                                 std::to_string(count));
    }
    const float* data = result.front().GetTensorData<float>();
    return std::vector<float>(data, data + count);
}

// --- Requests ------------------------------------------------------------------------------------------------------

embed::Response enhance(const embed::EnhanceAudio& request, SpeechModel& model) {
    if (request.audio.rate != dfn::kSampleRate) {
        return embed::Failure{request.id, "bad_request", "Enhance Speech needs 48 kHz audio"};
    }
    auto samples = embed::readAudioBlock(request.audio);
    if (!samples) {
        return embed::Failure{request.id, "io", samples.error().message};
    }
    const auto started = std::chrono::steady_clock::now();
    const auto channels = static_cast<std::size_t>(request.audio.channels);
    const auto frames = static_cast<std::size_t>(request.audio.frames);
    std::vector<float> out(samples->size());
    std::vector<float> mono(frames);
    std::string failure;
    for (std::size_t c = 0; c < channels; ++c) {
        for (std::size_t f = 0; f < frames; ++f) {
            mono[f] = (*samples)[f * channels + c];
        }
        auto enhanced = dfn::enhanceChannel(mono, [&](const dfn::Features& features) -> std::expected<dfn::NetworkOutput, Error> {
            try {
                return model.run(features);
            } catch (const std::exception& e) {
                failure = e.what();
                return std::unexpected(Error(ErrorCode::DecoderError, e.what()));
            }
        });
        if (!enhanced) {
            return embed::Failure{request.id, "inference", failure.empty() ? enhanced.error().message : failure};
        }
        for (std::size_t f = 0; f < frames; ++f) {
            out[f * channels + c] = (*enhanced)[f];
        }
    }
    try {
        writeFloats(request.output, out);
    } catch (const std::exception& e) {
        return embed::Failure{request.id, "io", e.what()};
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    return embed::AudioDone{request.id, request.audio.frames, request.audio.channels, 1, ms, model.device()};
}

embed::Response separate(const embed::SeparateAudio& request, StemModel& model) {
    if (request.audio.channels != 2) {
        return embed::Failure{request.id, "bad_request", "stem separation needs stereo audio"};
    }
    if (request.audio.frames > model.segment()) {
        return embed::Failure{request.id, "bad_request",
                              "a stem segment has at most " + std::to_string(model.segment()) + " frames"};
    }
    auto samples = embed::readAudioBlock(request.audio);
    if (!samples) {
        return embed::Failure{request.id, "io", samples.error().message};
    }
    const auto started = std::chrono::steady_clock::now();
    std::vector<float> stems;
    try {
        stems = model.run(*samples, request.audio.frames);
    } catch (const std::exception& e) {
        return embed::Failure{request.id, "inference", e.what()};
    }
    try {
        writeFloats(request.output, stems);
    } catch (const std::exception& e) {
        return embed::Failure{request.id, "io", e.what()};
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    return embed::AudioDone{request.id, request.audio.frames, 2, model.stems(), ms, model.device()};
}

} // namespace wabcut::embed_audio
