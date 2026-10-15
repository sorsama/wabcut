#pragma once

// wabcut-embed's translation model (AI-12; docs/ai.md "Going global"): an OPUS-MT (Marian) ONNX export, encoder and
// decoder (without the key/value cache), with its tokenizer and config. Greedy decoding: the decoder runs on the
// whole prefix for each new token (captions are short), the pad token never chosen (config bad_words_ids), until
// </s> or the token budget. DirectML when the device allows and it works, else the CPU; a GPU failure while running
// switches to the CPU once.

#include "AudioModels.h" // SessionFactory, Graph

#include <wabcut/ai/search/EmbedProtocol.h>
#include <wabcut/ai/translate/MarianTokenizer.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace wabcut::embed_translate {

class TranslationModel {
public:
    /// `directory`: encoder_model_quantized.onnx, decoder_model_quantized.onnx, tokenizer.json, config.json. Throws
    /// std::invalid_argument (a file missing) or std::runtime_error (not a Marian export).
    TranslationModel(const std::filesystem::path& directory, const std::string& device, int threads, embed_audio::SessionFactory factory);

    /// The translation of one text, and how many tokens were generated. Throws on failure.
    std::string translate(const std::string& text, const std::string& targetToken, int maxTokens, int& generated);

    const std::filesystem::path& directory() const { return m_directory; }
    const std::string& device() const { return m_device; }

private:
    void open(bool gpu);
    std::vector<std::int64_t> generate(const std::vector<std::int64_t>& source, int maxTokens);

    std::filesystem::path m_directory;
    int m_threads = 0;
    embed_audio::SessionFactory m_factory;
    std::string m_device;
    ai::translate::MarianTokenizer m_tokenizer;
    embed_audio::Graph m_encoder;
    embed_audio::Graph m_decoder;
    std::int64_t m_decoderStart = 0;
    std::int64_t m_eos = 0;
    std::vector<std::int64_t> m_banned;
};

/// The answer to a translate_text request (Translations or Failure).
ai::search::embed::Response translate(const ai::search::embed::TranslateText& request, TranslationModel& model);

} // namespace wabcut::embed_translate
