// wabcut-embed: runs the smart-search model (SigLIP 2's image and text encoders, ONNX) out of the editor's process and
// speaks the NDJSON protocol of wabcut/ai/search/EmbedProtocol.h on stdin/stdout. One request at a time; the parent
// kills the helper to cancel. Logs go to stderr, which the parent sends to a file or discards.
//
// ONNX Runtime comes from Microsoft's official binaries (cmake/ports/onnxruntime-prebuilt). Its telemetry is disabled
// before any session exists (Ort::Env::DisableTelemetryEvents; on Windows the official build would otherwise report
// session events to Microsoft's ETW provider). On Windows the DirectML provider runs the model on the GPU when the
// device allows ("auto"/"gpu"); if DirectML cannot create the session or fails while running, the helper falls back
// to the CPU provider and says so in "loaded"/stderr. Other systems use the CPU provider.
//
// Preprocessing matches SiglipImageProcessor / SiglipProcessor: pictures arrive resized to 224 x 224 (the parent
// resamples like Pillow's bilinear filter), are rescaled to [0, 1] and normalised with mean = std = 0.5, NCHW float32;
// texts are tokenized by the Gemma BPE tokenizer (lowercased, <eos>, padded to 64). The pooled outputs are
// L2-normalised.
//
// AI-9 adds subject detection for auto reframe: YuNet (faces) and YOLOX (people), one picture per run, with the
// letterboxing and decoding of wabcut/ai/vision/Detection.h (so the editor's tests cover them on fixture tensors).
//
// A.4 adds the audio models (AudioModels.h): Enhance Speech (DeepFilterNet3) and stem separation (HTDemucs).

#include "AudioModels.h"
#include "SegmentModels.h"
#include "TranslateModels.h"

#include <wabcut/ai/search/EmbedProtocol.h>
#include <wabcut/ai/search/Tokenizer.h>
#include <wabcut/ai/vision/Detection.h>

#include <onnxruntime_cxx_api.h>
#ifdef _WIN32
#include <dml_provider_factory.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <wabcut/platform/DllSearch.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace embed = wabcut::ai::search::embed;
namespace vision = wabcut::ai::vision;
using wabcut::ai::search::BpeTokenizer;
using wabcut::ai::search::Vector;

namespace {

void send(const embed::Response& response) {
    std::string line = embed::encode(response);
    line += '\n';
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fflush(stdout);
}

void log(const std::string& text) {
    std::fprintf(stderr, "wabcut-embed: %s\n", text.c_str());
    std::fflush(stderr);
}

std::string pathText(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

/// One ONNX model with the provider it runs on.
struct Model {
    std::unique_ptr<Ort::Session> session;
    std::string inputName;
    std::string outputName;
    std::string device;
    int dimension = 0;
};

/// A detector: one input, any number of named outputs.
struct Net {
    std::unique_ptr<Ort::Session> session;
    std::string inputName;
    std::vector<std::string> outputNames;
    std::string device;
};

class Engine {
public:
    Engine() : m_env(ORT_LOGGING_LEVEL_WARNING, "wabcut-embed") {
        m_env.DisableTelemetryEvents(); // before any session: nothing is reported
    }

    std::vector<std::string> providers() const { return Ort::GetAvailableProviders(); }

    /// Loads `file` on the GPU provider when `device` allows and it works, else on the CPU.
    Model load(const std::filesystem::path& file, const std::string& device, int threads, const char* preferredOutput) {
        if (!std::filesystem::is_regular_file(file)) {
            throw std::invalid_argument("model file missing: " + pathText(file));
        }
        std::optional<Model> model;
#ifdef _WIN32
        if (device != "cpu" && hasProvider("DmlExecutionProvider")) {
            try {
                model = open(file, true, threads, preferredOutput);
            } catch (const std::exception& e) {
                log("DirectML could not load " + pathText(file.filename()) + " (" + e.what() + "); using the CPU");
            }
        }
#endif
        if (!model) {
            model = open(file, false, threads, preferredOutput);
        }
        return std::move(*model);
    }

    /// Loads a detector like load().
    Net loadNet(const std::filesystem::path& file, const std::string& device, int threads) {
        if (!std::filesystem::is_regular_file(file)) {
            throw std::invalid_argument("model file missing: " + pathText(file));
        }
        std::optional<Net> net;
#ifdef _WIN32
        if (device != "cpu" && hasProvider("DmlExecutionProvider")) {
            try {
                net = openNet(file, true, threads);
            } catch (const std::exception& e) {
                log("DirectML could not load " + pathText(file.filename()) + " (" + e.what() + "); using the CPU");
            }
        }
#endif
        if (!net) {
            net = openNet(file, false, threads);
        }
        return std::move(*net);
    }

    /// A session on the GPU (DirectML) or the CPU for the audio models (A.4); `device` receives which. Throws.
    std::unique_ptr<Ort::Session> makeSession(const std::filesystem::path& file, bool gpu, int threads, std::string& device) {
        if (gpu && !hasProvider("DmlExecutionProvider")) {
            throw std::runtime_error("no DirectML provider");
        }
        return session(file, gpu, threads, device);
    }

    void fallBack(Net& net, const std::filesystem::path& file, int threads) {
        log("DirectML failed while running; switching " + pathText(file.filename()) + " to the CPU");
        net = openNet(file, false, threads);
    }

    /// Reloads `model` on the CPU (after a GPU failure while running).
    void fallBack(Model& model, const std::filesystem::path& file, int threads, const char* preferredOutput) {
        log("DirectML failed while running; switching " + pathText(file.filename()) + " to the CPU");
        model = open(file, false, threads, preferredOutput);
    }

private:
    bool hasProvider(const std::string& name) const {
        const auto all = providers();
        return std::ranges::find(all, name) != all.end();
    }

    /// A session on the GPU (DirectML) or the CPU; `device` receives which.
    std::unique_ptr<Ort::Session> session(const std::filesystem::path& file, bool gpu, int threads, std::string& device) {
        Ort::SessionOptions options;
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        if (threads > 0) {
            options.SetIntraOpNumThreads(threads);
        }
        device = "CPU";
#ifdef _WIN32
        if (gpu) {
            // DirectML's requirements: no memory pattern, sequential execution.
            options.DisableMemPattern();
            options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
            const OrtDmlApi* dml = nullptr;
            Ort::ThrowOnError(Ort::GetApi().GetExecutionProviderApi("DML", ORT_API_VERSION, reinterpret_cast<const void**>(&dml)));
            Ort::ThrowOnError(dml->SessionOptionsAppendExecutionProvider_DML(options, 0));
            device = "DirectML";
        }
#else
        (void)gpu;
#endif
        return std::make_unique<Ort::Session>(m_env, file.c_str(), options);
    }

    Net openNet(const std::filesystem::path& file, bool gpu, int threads) {
        Net net;
        net.session = session(file, gpu, threads, net.device);
        Ort::AllocatorWithDefaultOptions allocator;
        if (net.session->GetInputCount() != 1) {
            throw std::runtime_error("the detector has " + std::to_string(net.session->GetInputCount()) + " inputs, not 1");
        }
        net.inputName = net.session->GetInputNameAllocated(0, allocator).get();
        for (std::size_t i = 0; i < net.session->GetOutputCount(); ++i) {
            net.outputNames.emplace_back(net.session->GetOutputNameAllocated(i, allocator).get());
        }
        return net;
    }

    Model open(const std::filesystem::path& file, bool gpu, int threads, const char* preferredOutput) {
        Model model;
        model.session = session(file, gpu, threads, model.device);
        Ort::AllocatorWithDefaultOptions allocator;
        if (model.session->GetInputCount() != 1) {
            throw std::runtime_error("the model has " + std::to_string(model.session->GetInputCount()) + " inputs, not 1");
        }
        model.inputName = model.session->GetInputNameAllocated(0, allocator).get();
        const std::size_t outputs = model.session->GetOutputCount();
        for (std::size_t i = 0; i < outputs; ++i) {
            const std::string name = model.session->GetOutputNameAllocated(i, allocator).get();
            const auto shape = model.session->GetOutputTypeInfo(i).GetTensorTypeAndShapeInfo().GetShape();
            if (shape.size() == 2 && (model.outputName.empty() || name == preferredOutput)) {
                model.outputName = name;
                model.dimension = static_cast<int>(shape[1]);
            }
        }
        if (model.outputName.empty() || model.dimension <= 0) {
            throw std::runtime_error("the model has no pooled [batch, dimension] output");
        }
        return model;
    }

    Ort::Env m_env;
};

/// The pooled outputs of one run, L2-normalised.
std::vector<Vector> pooled(Ort::Value& output, std::size_t count, int dimension) {
    const float* data = output.GetTensorData<float>();
    std::vector<Vector> out;
    for (std::size_t i = 0; i < count; ++i) {
        Vector v(data + i * static_cast<std::size_t>(dimension), data + (i + 1) * static_cast<std::size_t>(dimension));
        wabcut::ai::search::normalize(v);
        out.push_back(std::move(v));
    }
    return out;
}

class Server {
public:
    void hello(const embed::Hello& h) {
        send(embed::HelloReply{embed::kProtocolVersion, "onnxruntime", Ort::GetVersionString(), m_engine.providers()});
        (void)h;
    }

    void load(const embed::Load& r) {
        try {
            if (!r.vision.empty() && !m_vision) {
                m_vision = m_engine.load(r.vision, r.device, r.threads, "pooler_output");
                m_visionFile = r.vision;
                m_threads = r.threads;
            }
            if (!r.text.empty() && !m_text) {
                auto tokenizer = BpeTokenizer::load(r.tokenizer);
                if (!tokenizer) {
                    send(embed::Failure{r.id, "model", tokenizer.error().message});
                    return;
                }
                m_tokenizer = std::move(*tokenizer);
                m_text = m_engine.load(r.text, r.device, r.threads, "pooler_output");
                m_textFile = r.text;
                m_threads = r.threads;
            }
            if (!r.faces.empty() && !m_faces) {
                Net faces = m_engine.loadNet(r.faces, r.device, r.threads);
                for (const char* name : {"cls_8", "obj_8", "bbox_8", "kps_8", "cls_32", "kps_32"}) {
                    if (std::ranges::find(faces.outputNames, name) == faces.outputNames.end()) {
                        throw std::runtime_error(std::string("the face detector has no output '") + name + "' (not YuNet 2023mar?)");
                    }
                }
                m_faces = std::move(faces);
                m_facesFile = r.faces;
                m_threads = r.threads;
            }
            if (!r.objects.empty() && !m_objects) {
                m_objects = m_engine.loadNet(r.objects, r.device, r.threads);
                m_objectsFile = r.objects;
                m_threads = r.threads;
            }
            const auto factory = [this](const std::filesystem::path& file, bool gpu, int threads, std::string& device) {
                return m_engine.makeSession(file, gpu, threads, device);
            };
            if (!r.speech.empty() && !m_speech) {
                m_speech.emplace(r.speech, r.device, r.threads, factory);
            }
            if (!r.stems.empty() && !m_stems) {
                m_stems.emplace(r.stems, r.device, r.threads, factory);
            }
            if (!r.translation.empty() && (!m_translation || m_translation->directory() != r.translation)) {
                m_translation.reset(); // one translation model at a time (AI-12)
                m_translation.emplace(r.translation, r.device, r.threads, factory);
            }
            if (!r.segmentation.empty() && (!m_segment || m_segment->directory() != r.segmentation)) {
                m_segment.reset(); // AI-14: SAM 2.1's video tracker, one object at a time
                m_segment.emplace(r.segmentation, r.device, r.threads, factory);
            }
        } catch (const std::invalid_argument& e) {
            send(embed::Failure{r.id, "model", e.what()});
            return;
        } catch (const std::exception& e) {
            send(embed::Failure{r.id, "model", std::string("cannot load the model: ") + e.what()});
            return;
        }
        if (!m_vision && !m_text) { // detectors or audio models only
            const std::string device = !r.speech.empty() && m_speech             ? m_speech->device()
                                       : !r.stems.empty() && m_stems            ? m_stems->device()
                                       : !r.translation.empty() && m_translation ? m_translation->device()
                                       : !r.segmentation.empty() && m_segment    ? m_segment->device()
                                       : m_faces                     ? m_faces->device
                                       : m_objects                   ? m_objects->device
                                                                     : std::string("CPU");
            send(embed::Loaded{r.id, device, 0});
            return;
        }
        const Model& model = m_vision ? *m_vision : *m_text;
        if (m_vision && m_text && m_vision->dimension != m_text->dimension) {
            send(embed::Failure{r.id, "model", "the image and text models disagree on the vector dimension"});
            return;
        }
        send(embed::Loaded{r.id, model.device, model.dimension});
    }

    void images(const embed::EmbedImages& r) {
        if (!m_vision) {
            send(embed::Failure{r.id, "bad_request", "no image model loaded"});
            return;
        }
        auto pictures = embed::readImageBatch(r.images);
        if (!pictures) {
            send(embed::Failure{r.id, "io", pictures.error().message});
            return;
        }
        const auto n = static_cast<std::int64_t>(pictures->size());
        const std::int64_t h = r.images.height;
        const std::int64_t w = r.images.width;
        std::vector<float> input(static_cast<std::size_t>(n * 3 * h * w));
        for (std::int64_t i = 0; i < n; ++i) {
            const auto& rgb = (*pictures)[static_cast<std::size_t>(i)].rgb;
            for (std::int64_t y = 0; y < h; ++y) {
                for (std::int64_t x = 0; x < w; ++x) {
                    for (std::int64_t c = 0; c < 3; ++c) {
                        const float value = static_cast<float>(rgb[static_cast<std::size_t>((y * w + x) * 3 + c)]);
                        input[static_cast<std::size_t>(((i * 3 + c) * h + y) * w + x)] = value / 127.5f - 1.0f; // (v/255 - 0.5)/0.5
                    }
                }
            }
        }
        const std::array<std::int64_t, 4> shape{n, 3, h, w};
        run(r.id, *m_vision, m_visionFile, Ort::Value::CreateTensor<float>(m_memory, input.data(), input.size(), shape.data(), shape.size()),
            static_cast<std::size_t>(n));
    }

    void texts(const embed::EmbedText& r) {
        if (!m_text || !m_tokenizer) {
            send(embed::Failure{r.id, "bad_request", "no text model loaded"});
            return;
        }
        std::vector<std::int64_t> ids;
        for (const std::string& text : r.texts) {
            const auto encoded = m_tokenizer->encode(text); // lowercased, <eos>, padded to 64
            ids.insert(ids.end(), encoded.begin(), encoded.end());
        }
        const auto n = static_cast<std::int64_t>(r.texts.size());
        const std::array<std::int64_t, 2> shape{n, static_cast<std::int64_t>(ids.size()) / n};
        run(r.id, *m_text, m_textFile, Ort::Value::CreateTensor<std::int64_t>(m_memory, ids.data(), ids.size(), shape.data(), shape.size()),
            static_cast<std::size_t>(n));
    }

    void detect(const embed::Detect& r) {
        if ((r.faces && !m_faces) || (r.objects && !m_objects)) {
            send(embed::Failure{r.id, "bad_request", r.faces && !m_faces ? "no face detector loaded" : "no object detector loaded"});
            return;
        }
        auto pictures = embed::readImageBatch(r.images);
        if (!pictures) {
            send(embed::Failure{r.id, "io", pictures.error().message});
            return;
        }
        const auto started = std::chrono::steady_clock::now();
        embed::Detections reply{r.id, {}, 0.0};
        for (const auto& picture : *pictures) {
            embed::DetectedFrame frame;
            if (r.faces) {
                vision::Letterbox box;
                std::vector<float> input = vision::yunetInput(picture, box);
                auto outputs = infer(*m_faces, m_facesFile, input, vision::kYuNetSize);
                if (!outputs) {
                    send(embed::Failure{r.id, "inference", m_error});
                    return;
                }
                const auto named = [&](const std::string& name) -> std::span<const float> {
                    const auto it = std::ranges::find(m_faces->outputNames, name);
                    if (it == m_faces->outputNames.end()) {
                        return {};
                    }
                    Ort::Value& value = (*outputs)[static_cast<std::size_t>(it - m_faces->outputNames.begin())];
                    return {value.GetTensorData<float>(), value.GetTensorTypeAndShapeInfo().GetElementCount()};
                };
                vision::YuNetOutputs yunet;
                const char* strides[3] = {"8", "16", "32"};
                for (std::size_t s = 0; s < 3; ++s) {
                    yunet.cls[s] = named(std::string("cls_") + strides[s]);
                    yunet.obj[s] = named(std::string("obj_") + strides[s]);
                    yunet.bbox[s] = named(std::string("bbox_") + strides[s]);
                    yunet.kps[s] = named(std::string("kps_") + strides[s]);
                }
                for (const vision::Detection& d : vision::decodeYuNet(yunet, box, r.faceThreshold, 0.3f)) {
                    if (frame.faces.size() >= embed::kMaxDetectionsPerFrame) {
                        break;
                    }
                    std::vector<float> row{d.box.x, d.box.y, d.box.w, d.box.h, d.score};
                    row.insert(row.end(), d.landmarks.begin(), d.landmarks.end());
                    row.resize(embed::kFaceRow, 0.0f);
                    frame.faces.push_back(std::move(row));
                }
            }
            if (r.objects) {
                vision::Letterbox box;
                std::vector<float> input = vision::yoloxInput(picture, box);
                auto outputs = infer(*m_objects, m_objectsFile, input, vision::kYoloxSize);
                if (!outputs) {
                    send(embed::Failure{r.id, "inference", m_error});
                    return;
                }
                Ort::Value& value = outputs->front();
                const std::span<const float> raw(value.GetTensorData<float>(), value.GetTensorTypeAndShapeInfo().GetElementCount());
                for (const vision::Detection& d : vision::decodeYolox(raw, box, r.allClasses ? -1 : vision::kCocoPerson, r.objectThreshold, 0.45f)) {
                    if (frame.objects.size() >= embed::kMaxDetectionsPerFrame) {
                        break;
                    }
                    frame.objects.push_back({d.box.x, d.box.y, d.box.w, d.box.h, d.score, static_cast<float>(d.classId)});
                }
            }
            reply.frames.push_back(std::move(frame));
        }
        reply.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        send(reply);
    }

    void enhance(const embed::EnhanceAudio& r) {
        if (!m_speech) {
            send(embed::Failure{r.id, "bad_request", "no speech model loaded"});
            return;
        }
        send(wabcut::embed_audio::enhance(r, *m_speech));
    }

    void separate(const embed::SeparateAudio& r) {
        if (!m_stems) {
            send(embed::Failure{r.id, "bad_request", "no stem model loaded"});
            return;
        }
        send(wabcut::embed_audio::separate(r, *m_stems));
    }

    void segment(const embed::SegmentFrame& r) {
        if (!m_segment) {
            send(embed::Failure{r.id, "bad_request", "no segmentation model loaded"});
            return;
        }
        send(wabcut::embed_segment::segment(r, *m_segment));
    }

    void translate(const embed::TranslateText& r) {
        if (!m_translation) {
            send(embed::Failure{r.id, "bad_request", "no translation model loaded"});
            return;
        }
        send(wabcut::embed_translate::translate(r, *m_translation));
    }

private:
    /// Every output of one run of `net` on a 1 x 3 x size x size input; on the GPU first, then the CPU. Nothing (and
    /// m_error) when both fail.
    std::optional<std::vector<Ort::Value>> infer(Net& net, const std::filesystem::path& file, std::vector<float>& input, int size) {
        const std::array<std::int64_t, 4> shape{1, 3, size, size};
        for (int attempt = 0; attempt < 2; ++attempt) {
            try {
                Ort::Value tensor = Ort::Value::CreateTensor<float>(m_memory, input.data(), input.size(), shape.data(), shape.size());
                const char* inputs[] = {net.inputName.c_str()};
                std::vector<const char*> outputs;
                for (const std::string& name : net.outputNames) {
                    outputs.push_back(name.c_str());
                }
                return net.session->Run(Ort::RunOptions{nullptr}, inputs, &tensor, 1, outputs.data(), outputs.size());
            } catch (const std::exception& e) {
                if (attempt == 0 && net.device != "CPU") {
                    try {
                        m_engine.fallBack(net, file, m_threads);
                        continue;
                    } catch (const std::exception& reload) {
                        m_error = std::string("the detector failed on the GPU and the CPU: ") + reload.what();
                        return std::nullopt;
                    }
                }
                m_error = e.what();
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    void run(const std::string& id, Model& model, const std::filesystem::path& file, Ort::Value input, std::size_t count) {
        for (int attempt = 0; attempt < 2; ++attempt) {
            try {
                const auto started = std::chrono::steady_clock::now();
                const char* inputs[] = {model.inputName.c_str()};
                const char* outputs[] = {model.outputName.c_str()};
                auto result = model.session->Run(Ort::RunOptions{nullptr}, inputs, &input, 1, outputs, 1);
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
                send(embed::Embeddings{id, pooled(result.front(), count, model.dimension), ms});
                return;
            } catch (const std::exception& e) {
                if (attempt == 0 && model.device != "CPU") {
                    try {
                        m_engine.fallBack(model, file, m_threads, "pooler_output");
                        continue;
                    } catch (const std::exception& reload) {
                        send(embed::Failure{id, "inference", std::string("the model failed on the GPU and the CPU: ") + reload.what()});
                        return;
                    }
                }
                send(embed::Failure{id, "inference", e.what()});
                return;
            }
        }
    }

    Engine m_engine;
    Ort::MemoryInfo m_memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    std::optional<Model> m_vision;
    std::optional<Model> m_text;
    std::optional<BpeTokenizer> m_tokenizer;
    std::optional<Net> m_faces;
    std::optional<Net> m_objects;
    std::optional<wabcut::embed_audio::SpeechModel> m_speech;
    std::optional<wabcut::embed_audio::StemModel> m_stems;
    std::optional<wabcut::embed_translate::TranslationModel> m_translation;
    std::optional<wabcut::embed_segment::Sam2Tracker> m_segment;
    std::filesystem::path m_facesFile;
    std::filesystem::path m_objectsFile;
    std::string m_error;
    std::filesystem::path m_visionFile;
    std::filesystem::path m_textFile;
    int m_threads = 0;
};

} // namespace

int main() {
    wabcut::platform::hardenDllSearch(); // first: no DLL from the current directory or PATH (S5-P2-5)
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    std::unique_ptr<Server> server;
    try {
        server = std::make_unique<Server>();
    } catch (const std::exception& e) {
        log(std::string("ONNX Runtime did not start: ") + e.what());
        return 2;
    }
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        auto request = embed::decodeRequest(line);
        if (!request) {
            send(embed::Failure{"", "protocol", request.error().message});
            continue;
        }
        try {
            if (std::holds_alternative<embed::Shutdown>(*request)) {
                return 0;
            } else if (const auto* hello = std::get_if<embed::Hello>(&*request)) {
                server->hello(*hello);
            } else if (const auto* load = std::get_if<embed::Load>(&*request)) {
                server->load(*load);
            } else if (const auto* images = std::get_if<embed::EmbedImages>(&*request)) {
                server->images(*images);
            } else if (const auto* texts = std::get_if<embed::EmbedText>(&*request)) {
                server->texts(*texts);
            } else if (const auto* detect = std::get_if<embed::Detect>(&*request)) {
                server->detect(*detect);
            } else if (const auto* enhance = std::get_if<embed::EnhanceAudio>(&*request)) {
                server->enhance(*enhance);
            } else if (const auto* separate = std::get_if<embed::SeparateAudio>(&*request)) {
                server->separate(*separate);
            } else if (const auto* translate = std::get_if<embed::TranslateText>(&*request)) {
                server->translate(*translate);
            } else if (const auto* segment = std::get_if<embed::SegmentFrame>(&*request)) {
                server->segment(*segment);
            }
        } catch (const std::exception& e) {
            send(embed::Failure{"", "inference", e.what()});
        }
    }
    return 0; // end of input: the editor went away
}
