#pragma once

// OPUS-MT's tokenizer (AI-12) from the tokenizer.json of its ONNX export: a SentencePiece unigram model as Hugging Face
// tokenizers writes it (vocab = [[piece, score], ...], the index is the id). Encoding splits the text at whitespace,
// marks each word's start with "▁" (Metaspace) and picks the pieces of each word with the highest total score
// (Viterbi); characters no piece covers become <unk>; "</s>" ends the sequence and a multilingual model's target token
// (">>tha<<") goes first. Decoding joins the pieces, "▁" read as a space, special and target tokens dropped. Pieces of
// the target vocabulary only (score -100 in these files) lose against every source piece, as in Marian's own split.

#include <wabcut/model/Error.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace wabcut::ai::translate {

class MarianTokenizer {
public:
    /// Errors: IoError (unreadable), ParseError (not a unigram tokenizer.json, or no </s>, <unk> or <pad>).
    static std::expected<MarianTokenizer, Error> load(const std::filesystem::path& file);
    static std::expected<MarianTokenizer, Error> fromJson(std::string_view json);

    std::vector<std::int64_t> encode(std::string_view text, std::string_view targetToken = {}) const;
    std::string decode(std::span<const std::int64_t> ids) const;

    std::optional<std::int64_t> idOf(std::string_view piece) const;
    const std::string& piece(std::int64_t id) const;
    std::size_t size() const { return m_pieces.size(); }
    std::int64_t eos() const { return m_eos; }
    std::int64_t unk() const { return m_unk; }
    std::int64_t pad() const { return m_pad; }

private:
    void encodeWord(std::string_view word, std::vector<std::int64_t>& out) const;

    std::vector<std::string> m_pieces;
    std::vector<double> m_scores;
    std::unordered_map<std::string, std::int64_t> m_ids;  ///< every piece
    std::unordered_map<std::string, std::int64_t> m_split; ///< the pieces words are split into (no special tokens)
    std::size_t m_maxPieceBytes = 0;
    double m_unkScore = -110.0;
    std::int64_t m_eos = 0;
    std::int64_t m_unk = 1;
    std::int64_t m_pad = 0;
};

} // namespace wabcut::ai::translate
