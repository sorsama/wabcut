#pragma once

// The SigLIP 2 text tokenizer: Gemma's byte-fallback BPE, read from a Hugging Face tokenizer.json.
//
// Pipeline (what the tokenizers library does with SigLIP 2's file, plus the processor's settings): lowercase (SigLIP 2
// was trained on lowercased text), every " " becomes "▁" (no prefix), BPE over the code points (a code point without a
// vocabulary entry becomes its UTF-8 bytes, <0xNN>), merges applied lowest rank first, leftmost first; then at most
// maxLength - 1 ids, <eos>, and <pad> up to maxLength. The 34 MB file is streamed (SAX), never held as a DOM.
//
// Only SigLIP 2's configuration is accepted: model type BPE, the " " -> "▁" Replace normalizer (or none) and no
// pre-tokenizer beyond a split on " " (which the normalizer has already removed). Anything else is Unsupported, not a
// silently different tokenization. Special-token text typed by the user ("<eos>") is tokenized as text.

#include <wabcut/model/Error.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace wabcut::ai::search {

struct TokenizerOptions {
    bool lowercase = true;
    std::size_t maxLength = 64; ///< ids including <eos>; 0 = no limit and no padding
    bool pad = true;            ///< pad with <pad> up to maxLength
};

class BpeTokenizer {
public:
    /// Errors: NotFound, IoError, ParseError (not JSON, no BPE model, no <eos>/<pad>), Unsupported (another
    /// normalizer or pre-tokenizer).
    static std::expected<BpeTokenizer, Error> load(const std::filesystem::path& file);
    static std::expected<BpeTokenizer, Error> fromJson(std::string_view json);

    std::vector<std::int64_t> encode(std::string_view text, const TokenizerOptions& options = {}) const;

    std::size_t vocabularySize() const { return m_vocab.size(); }
    std::size_t mergeCount() const { return m_merges.size(); }
    std::int32_t eosId() const { return m_eos; }
    std::int32_t padId() const { return m_pad; }

    struct Builder; // parsing state (Tokenizer.cpp)

private:
    std::unordered_map<std::string, std::int32_t> m_vocab;
    /// (left id << 32 | right id) -> (rank, merged id)
    std::unordered_map<std::uint64_t, std::pair<std::int32_t, std::int32_t>> m_merges;
    std::array<std::int32_t, 256> m_byteIds{};
    std::int32_t m_unk = -1;
    std::int32_t m_eos = -1;
    std::int32_t m_pad = -1;
};

/// Lowercases the letters of the scripts SigLIP 2 users type most (ASCII, Latin-1, Latin Extended-A, Greek,
/// Cyrillic); other text (Thai has no case) is unchanged. Invalid UTF-8 is kept byte for byte.
std::string lowercaseUtf8(std::string_view text);

} // namespace wabcut::ai::search
