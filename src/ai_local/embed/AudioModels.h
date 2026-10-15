#pragma once

// wabcut-embed's audio models (A.4; docs/ai.md "Enhance Speech" and "Stem separation"): DeepFilterNet3 (three ONNX
// graphs; its signal processing is wabcut/ai/audio/DeepFilter.h) and HTDemucs (one graph, waveform in, stems out).
// They run on the session factory of the helper's engine: DirectML when the device allows and it works, else the
// CPU, and a GPU failure while running switches to the CPU once.

#include <wabcut/ai/audio/DeepFilter.h>
#include <wabcut/ai/search/EmbedProtocol.h>

#include <onnxruntime_cxx_api.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace wabcut::embed_audio {

/// Creates a session for `file` on the GPU (`gpu`) or the CPU; `device` receives which ("DirectML", "CPU"). Throws.
using SessionFactory =
    std::function<std::unique_ptr<Ort::Session>(const std::filesystem::path& file, bool gpu, int threads, std::string& device)>;

/// One ONNX graph with its input and output names.
struct Graph {
    std::filesystem::path file;
    std::unique_ptr<Ort::Session> session;
    std::vector<std::string> inputs;
    std::vector<std::string> outputs;
};

/// DeepFilterNet3: enc.onnx, erb_dec.onnx and df_dec.onnx of one directory.
class SpeechModel {
public:
    /// Throws std::invalid_argument (a file missing) or std::runtime_error (not DeepFilterNet3's graphs).
    SpeechModel(const std::filesystem::path& directory, const std::string& device, int threads, SessionFactory factory);

    /// The networks over a block of frames. Throws on failure (after one retry on the CPU when the GPU failed).
    ai::audio::dfn::NetworkOutput run(const ai::audio::dfn::Features& features);
    const std::string& device() const { return m_device; }

private:
    void open(bool gpu);
    ai::audio::dfn::NetworkOutput runOnce(const ai::audio::dfn::Features& features);

    std::filesystem::path m_directory;
    int m_threads = 0;
    SessionFactory m_factory;
    std::string m_device;
    Graph m_enc;
    Graph m_erbDec;
    Graph m_dfDec;
};

/// HTDemucs (an ONNX export with the STFT inside): "mix" [1, 2, segment] -> "stems" [1, stems, 2, segment].
class StemModel {
public:
    StemModel(const std::filesystem::path& file, const std::string& device, int threads, SessionFactory factory);

    /// Stems of one stereo segment (frames x 2 interleaved, at most segment() frames; shorter ones are padded):
    /// stems() blocks of frames x 2, one after the other. Throws on failure.
    std::vector<float> run(const std::vector<float>& interleaved, int frames);
    int segment() const { return m_segment; }
    int stems() const { return m_stems; }
    const std::string& device() const { return m_device; }

private:
    void open(bool gpu);
    std::vector<float> runOnce(const std::vector<float>& planar);

    int m_threads = 0;
    SessionFactory m_factory;
    std::string m_device;
    Graph m_graph;
    int m_segment = 0;
    int m_stems = 0;
};

/// The answers to the requests (AudioDone or Failure), the output file written.
ai::search::embed::Response enhance(const ai::search::embed::EnhanceAudio& request, SpeechModel& model);
ai::search::embed::Response separate(const ai::search::embed::SeparateAudio& request, StemModel& model);

} // namespace wabcut::embed_audio
