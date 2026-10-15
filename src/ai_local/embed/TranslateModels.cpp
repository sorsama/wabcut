#include "TranslateModels.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace wabcut::embed_translate {

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

embed_audio::Graph openGraph(const std::filesystem::path& file, bool gpu, int threads, const embed_audio::SessionFactory& factory,
                             std::string& device) {
    if (!std::filesystem::is_regular_file(file)) {
        throw std::invalid_argument("model file missing: " + pathText(file));
    }
    embed_audio::Graph g;
    g.file = file;
    g.session = factory(file, gpu, threads, device);
    Ort::AllocatorWithDefaultOptions allocator;
    for (std::size_t i = 0; i < g.session->GetInputCount(); ++i) {
        g.inputs.emplace_back(g.session->GetInputNameAllocated(i, allocator).get());
    }
    for (std::size_t i = 0; i < g.session->GetOutputCount(); ++i) {
        g.outputs.emplace_back(g.session->GetOutputNameAllocated(i, allocator).get());
    }
    return g;
}

bool has(const std::vector<std::string>& names, std::string_view name) { return std::ranges::find(names, std::string(name)) != names.end(); }

Ort::MemoryInfo& cpuMemory() {
    static Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    return memory;
}

nlohmann::json readJson(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        throw std::invalid_argument("model file missing: " + pathText(file));
    }
    std::ostringstream text;
    text << in.rdbuf();
    nlohmann::json j = nlohmann::json::parse(text.str(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        throw std::runtime_error(pathText(file.filename()) + " is not JSON");
    }
    return j;
}

} // namespace

TranslationModel::TranslationModel(const std::filesystem::path& directory, const std::string& device, int threads,
                                   embed_audio::SessionFactory factory)
    : m_directory(directory), m_threads(threads), m_factory(std::move(factory)) {
    auto tokenizer = ai::translate::MarianTokenizer::load(directory / "tokenizer.json");
    if (!tokenizer) {
        if (!std::filesystem::is_regular_file(directory / "tokenizer.json")) {
            throw std::invalid_argument("model file missing: " + pathText(directory / "tokenizer.json"));
        }
        throw std::runtime_error(tokenizer.error().message);
    }
    m_tokenizer = std::move(*tokenizer);
    const nlohmann::json config = readJson(directory / "config.json");
    if (config.value("model_type", std::string()) != "marian") {
        throw std::runtime_error("config.json is not a Marian model's");
    }
    m_decoderStart = config.value("decoder_start_token_id", m_tokenizer.pad());
    m_eos = config.value("eos_token_id", m_tokenizer.eos());
    m_banned.push_back(m_tokenizer.pad());
    if (const auto bad = config.find("bad_words_ids"); bad != config.end() && bad->is_array()) {
        for (const auto& words : *bad) {
            if (words.is_array() && words.size() == 1 && words[0].is_number_integer()) {
                m_banned.push_back(words[0].get<std::int64_t>());
            }
        }
    }
    bool opened = false;
#ifdef _WIN32
    if (device != "cpu") {
        try {
            open(true);
            opened = true;
        } catch (const std::invalid_argument&) {
            throw;
        } catch (const std::exception& e) {
            log(std::string("DirectML could not load the translation model (") + e.what() + "); using the CPU");
        }
    }
#else
    (void)device;
#endif
    if (!opened) {
        open(false);
    }
}

void TranslationModel::open(bool gpu) {
    std::string device;
    m_encoder = openGraph(m_directory / "encoder_model_quantized.onnx", gpu, m_threads, m_factory, device);
    m_decoder = openGraph(m_directory / "decoder_model_quantized.onnx", gpu, m_threads, m_factory, device);
    m_device = device;
    if (!has(m_encoder.inputs, "input_ids") || !has(m_encoder.inputs, "attention_mask") || !has(m_encoder.outputs, "last_hidden_state")) {
        throw std::runtime_error("the encoder is not a Marian export (input_ids, attention_mask -> last_hidden_state)");
    }
    if (!has(m_decoder.inputs, "input_ids") || !has(m_decoder.inputs, "encoder_hidden_states") || !has(m_decoder.outputs, "logits")) {
        throw std::runtime_error("the decoder is not a Marian export without a cache (input_ids, encoder_hidden_states -> logits)");
    }
    log("translation model " + pathText(m_directory.filename()) + " on " + m_device);
}

std::vector<std::int64_t> TranslationModel::generate(const std::vector<std::int64_t>& source, int maxTokens) {
    std::vector<std::int64_t> ids = source;
    std::vector<std::int64_t> mask(source.size(), 1);
    const std::array<std::int64_t, 2> sourceShape{1, static_cast<std::int64_t>(source.size())};
    std::array<Ort::Value, 2> encoderInputs{
        Ort::Value::CreateTensor<std::int64_t>(cpuMemory(), ids.data(), ids.size(), sourceShape.data(), sourceShape.size()),
        Ort::Value::CreateTensor<std::int64_t>(cpuMemory(), mask.data(), mask.size(), sourceShape.data(), sourceShape.size())};
    const char* encoderNames[] = {"input_ids", "attention_mask"};
    const char* hiddenName[] = {"last_hidden_state"};
    auto hidden = m_encoder.session->Run(Ort::RunOptions{nullptr}, encoderNames, encoderInputs.data(), encoderInputs.size(), hiddenName, 1);
    Ort::Value& states = hidden.front();
    const std::vector<std::int64_t> statesShape = states.GetTensorTypeAndShapeInfo().GetShape();
    float* statesData = states.GetTensorMutableData<float>();
    const std::size_t statesCount = states.GetTensorTypeAndShapeInfo().GetElementCount();

    // The decoder's inputs by name (exports differ in order).
    std::vector<const char*> names;
    for (const std::string& n : m_decoder.inputs) {
        names.push_back(n.c_str());
    }
    const char* logitsName[] = {"logits"};
    std::vector<std::int64_t> out{m_decoderStart};
    for (int step = 0; step < maxTokens; ++step) {
        const std::array<std::int64_t, 2> targetShape{1, static_cast<std::int64_t>(out.size())};
        std::vector<Ort::Value> inputs;
        for (const std::string& n : m_decoder.inputs) {
            if (n == "input_ids") {
                inputs.push_back(Ort::Value::CreateTensor<std::int64_t>(cpuMemory(), out.data(), out.size(), targetShape.data(), targetShape.size()));
            } else if (n == "encoder_attention_mask") {
                inputs.push_back(Ort::Value::CreateTensor<std::int64_t>(cpuMemory(), mask.data(), mask.size(), sourceShape.data(), sourceShape.size()));
            } else if (n == "encoder_hidden_states") {
                inputs.push_back(Ort::Value::CreateTensor<float>(cpuMemory(), statesData, statesCount, statesShape.data(), statesShape.size()));
            } else {
                throw std::runtime_error("the decoder wants an input this helper does not know: " + n);
            }
        }
        auto logits = m_decoder.session->Run(Ort::RunOptions{nullptr}, names.data(), inputs.data(), inputs.size(), logitsName, 1);
        const auto shape = logits.front().GetTensorTypeAndShapeInfo().GetShape();
        if (shape.size() != 3 || shape[1] != static_cast<std::int64_t>(out.size()) || shape[2] <= 0) {
            throw std::runtime_error("the decoder returned logits of an unexpected shape");
        }
        const std::int64_t vocabulary = shape[2];
        const float* last = logits.front().GetTensorData<float>() + (out.size() - 1) * static_cast<std::size_t>(vocabulary);
        std::int64_t next = -1;
        float bestScore = -std::numeric_limits<float>::infinity();
        for (std::int64_t v = 0; v < vocabulary; ++v) {
            if (last[v] > bestScore && std::ranges::find(m_banned, v) == m_banned.end()) {
                bestScore = last[v];
                next = v;
            }
        }
        if (next < 0 || next == m_eos) {
            break;
        }
        out.push_back(next);
        // A degenerate loop (the same token over and over) ends the line.
        if (out.size() > 8 && std::all_of(out.end() - 8, out.end(), [&](std::int64_t t) { return t == next; })) {
            break;
        }
    }
    out.erase(out.begin()); // the start token
    return out;
}

std::string TranslationModel::translate(const std::string& text, const std::string& targetToken, int maxTokens, int& generated) {
    const std::vector<std::int64_t> source = m_tokenizer.encode(text, targetToken);
    if (source.size() <= 1) {
        generated = 0;
        return {};
    }
    std::vector<std::int64_t> output;
    try {
        output = generate(source, maxTokens);
    } catch (const std::exception& e) {
        if (m_device == "CPU") {
            throw;
        }
        log(std::string("DirectML failed while translating (") + e.what() + "); switching to the CPU");
        open(false);
        output = generate(source, maxTokens);
    }
    generated = static_cast<int>(output.size());
    return m_tokenizer.decode(output);
}

embed::Response translate(const embed::TranslateText& request, TranslationModel& model) {
    const auto started = std::chrono::steady_clock::now();
    embed::Translations reply;
    reply.id = request.id;
    for (const std::string& text : request.texts) {
        int generated = 0;
        try {
            reply.texts.push_back(model.translate(text, request.target, request.maxTokens, generated));
        } catch (const std::exception& e) {
            return embed::Failure{request.id, "inference", std::string("translation failed: ") + e.what()};
        }
        reply.tokens += generated;
    }
    reply.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    reply.device = model.device();
    return reply;
}

} // namespace wabcut::embed_translate
