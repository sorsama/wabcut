#include <wabcut/ai/search/Tokenizer.h>

#include <wabcut/model/MediaLocator.h>

#include <nlohmann/json.hpp>

#include <climits>
#include <cstdio>
#include <fstream>
#include <map>

namespace wabcut::ai::search {

namespace {

using Json = nlohmann::json;

constexpr std::string_view kSpace = "\xE2\x96\x81"; // U+2581, SentencePiece's word-boundary mark
/// Longest text tokenized (bytes): far more than 64 tokens' worth; a pasted essay is cut, not tokenized whole.
constexpr std::size_t kMaxTextBytes = 8192;

/// Length of the UTF-8 sequence at s[i]; 0 when it is not a valid one.
std::size_t utf8Length(std::string_view s, std::size_t i) {
    const auto c = static_cast<unsigned char>(s[i]);
    std::size_t n = 0;
    if (c < 0x80) {
        return 1;
    }
    if ((c & 0xE0) == 0xC0 && c >= 0xC2) {
        n = 2;
    } else if ((c & 0xF0) == 0xE0) {
        n = 3;
    } else if ((c & 0xF8) == 0xF0 && c <= 0xF4) {
        n = 4;
    } else {
        return 0;
    }
    if (i + n > s.size()) {
        return 0;
    }
    for (std::size_t k = 1; k < n; ++k) {
        if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) {
            return 0;
        }
    }
    return n;
}

char32_t decode(std::string_view s, std::size_t i, std::size_t n) {
    const auto c = static_cast<unsigned char>(s[i]);
    char32_t cp = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
    for (std::size_t k = 1; k < n; ++k) {
        cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    }
    return cp;
}

void append(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

char32_t lower(char32_t cp) {
    if (cp >= U'A' && cp <= U'Z') {
        return cp + 32;
    }
    if (cp < 0xC0) {
        return cp;
    }
    if (cp <= 0xDE) {
        return cp == 0xD7 ? cp : cp + 32; // Latin-1 capitals (not ×)
    }
    if ((cp >= 0x100 && cp <= 0x137) || (cp >= 0x14A && cp <= 0x177)) {
        return cp % 2 == 0 ? cp + 1 : cp; // Latin Extended-A pairs, capital first
    }
    if ((cp >= 0x139 && cp <= 0x148) || (cp >= 0x179 && cp <= 0x17E)) {
        return cp % 2 == 1 ? cp + 1 : cp;
    }
    if (cp == 0x178) {
        return 0xFF; // Ÿ
    }
    if (cp >= 0x391 && cp <= 0x3A9 && cp != 0x3A2) {
        return cp + 32; // Greek
    }
    if (cp >= 0x410 && cp <= 0x42F) {
        return cp + 32; // Cyrillic
    }
    if (cp >= 0x400 && cp <= 0x40F) {
        return cp + 80;
    }
    return cp;
}

std::uint64_t pairKey(std::int32_t left, std::int32_t right) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(left)) << 32) | static_cast<std::uint32_t>(right);
}

std::unexpected<Error> fail(ErrorCode code, std::string message) {
    return std::unexpected(Error(code, "tokenizer: " + std::move(message)));
}

} // namespace

std::string lowercaseUtf8(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const std::size_t n = utf8Length(text, i);
        if (n == 0) {
            out += text[i++];
            continue;
        }
        append(out, lower(decode(text, i, n)));
        i += n;
    }
    return out;
}

// --- Loading ------------------------------------------------------------------------------------------------------

/// SAX handler: streams model.vocab and model.merges into the tokenizer and keeps the few settings it checks
/// (model.*, normalizer.*, pre_tokenizer.*) as text by dotted path.
struct BpeTokenizer::Builder : nlohmann::json_sax<Json> {
    struct Frame {
        bool array = false;
        std::string key;
        std::size_t index = 0;
    };

    BpeTokenizer& out;
    std::vector<Frame> stack;
    std::map<std::string, std::string> settings;
    std::vector<std::pair<std::string, std::string>> pendingMerges; // merges listed before the vocabulary
    std::vector<std::string> pair;
    std::int32_t rank = 0;
    std::size_t skippedMerges = 0;
    std::string error;

    explicit Builder(BpeTokenizer& tokenizer) : out(tokenizer) {}

    bool inModel(std::string_view key, std::size_t depth) const {
        return stack.size() == depth && !stack[0].array && stack[0].key == "model" && stack.size() >= 2 &&
               !stack[1].array && stack[1].key == key;
    }
    bool inVocab() const { return inModel("vocab", 3) && !stack[2].array; }
    bool inMergeString() const { return inModel("merges", 3) && stack[2].array; }
    bool inMergePair() const { return inModel("merges", 4) && stack[2].array && stack[3].array; }

    void record(std::string value) {
        if (stack.empty() || stack.size() > 3 || stack[0].array) {
            return;
        }
        const std::string& top = stack[0].key;
        if (top != "model" && top != "normalizer" && top != "pre_tokenizer") {
            return;
        }
        std::string path;
        for (const Frame& f : stack) {
            if (!path.empty()) {
                path += '.';
            }
            path += f.array ? std::to_string(f.index) : f.key;
        }
        settings[path] = std::move(value);
    }
    void advance() {
        if (!stack.empty() && stack.back().array) {
            ++stack.back().index;
        }
    }
    bool scalar(std::string value) {
        record(std::move(value));
        advance();
        return true;
    }

    void addMerge(const std::string& left, const std::string& right) {
        if (out.m_vocab.empty()) {
            pendingMerges.emplace_back(left, right);
            ++rank;
            return;
        }
        const auto l = out.m_vocab.find(left);
        const auto r = out.m_vocab.find(right);
        const auto m = out.m_vocab.find(left + right);
        if (l == out.m_vocab.end() || r == out.m_vocab.end() || m == out.m_vocab.end()) {
            ++skippedMerges;
        } else {
            out.m_merges.try_emplace(pairKey(l->second, r->second), rank, m->second);
        }
        ++rank;
    }

    bool null() override { return scalar("null"); }
    bool boolean(bool value) override { return scalar(value ? "true" : "false"); }
    bool number_integer(number_integer_t value) override {
        if (inVocab()) {
            if (value < 0 || value > INT32_MAX) {
                error = "a vocabulary id is out of range";
                return false;
            }
            out.m_vocab[stack.back().key] = static_cast<std::int32_t>(value);
            return true;
        }
        return scalar(std::to_string(value));
    }
    bool number_unsigned(number_unsigned_t value) override {
        if (inVocab()) {
            if (value > INT32_MAX) {
                error = "a vocabulary id is out of range";
                return false;
            }
            out.m_vocab[stack.back().key] = static_cast<std::int32_t>(value);
            return true;
        }
        return scalar(std::to_string(value));
    }
    bool number_float(number_float_t value, const string_t&) override { return scalar(std::to_string(value)); }
    bool string(string_t& value) override {
        if (inMergeString()) { // the older "left right" form
            const auto space = value.find(' ');
            if (space == std::string::npos) {
                error = "a merge is not \"left right\"";
                return false;
            }
            addMerge(value.substr(0, space), value.substr(space + 1));
            advance();
            return true;
        }
        if (inMergePair()) {
            pair.push_back(std::move(value));
            advance();
            return true;
        }
        if (inVocab()) {
            error = "a vocabulary id is not a number";
            return false;
        }
        return scalar(std::move(value));
    }
    bool binary(binary_t&) override { return scalar("binary"); }
    bool start_object(std::size_t) override {
        stack.push_back(Frame{false, {}, 0});
        return true;
    }
    bool key(string_t& value) override {
        stack.back().key = std::move(value);
        return true;
    }
    bool end_object() override {
        stack.pop_back();
        advance();
        return true;
    }
    bool start_array(std::size_t) override {
        if (inMergeString()) {
            pair.clear(); // a [left, right] pair starts
        }
        stack.push_back(Frame{true, {}, 0});
        return true;
    }
    bool end_array() override {
        const bool closesPair = inMergePair();
        stack.pop_back();
        if (closesPair) {
            if (pair.size() != 2) {
                error = "a merge is not a pair";
                return false;
            }
            addMerge(pair[0], pair[1]);
            pair.clear();
        }
        advance();
        return true;
    }
    bool parse_error(std::size_t position, const std::string&, const nlohmann::detail::exception& e) override {
        error = "not JSON at byte " + std::to_string(position) + ": " + e.what();
        return false;
    }

    std::string setting(const std::string& path) const {
        const auto it = settings.find(path);
        return it == settings.end() ? std::string("<absent>") : it->second;
    }

    std::expected<void, Error> finish() {
        if (setting("model.type") != "BPE") {
            return fail(ErrorCode::ParseError, "not a BPE tokenizer.json (model.type " + setting("model.type") + ")");
        }
        const bool spaceToMark = setting("normalizer.type") == "Replace" && setting("normalizer.pattern.String") == " " &&
                                 setting("normalizer.content") == kSpace;
        if (!spaceToMark) {
            return fail(ErrorCode::Unsupported, "normalizer " + setting("normalizer.type") + " is not SigLIP 2's");
        }
        const std::string pre = setting("pre_tokenizer.type");
        if (pre != "<absent>" && !(pre == "Split" && setting("pre_tokenizer.pattern.String") == " ")) {
            return fail(ErrorCode::Unsupported, "pre-tokenizer " + pre + " is not SigLIP 2's");
        }
        if (setting("model.ignore_merges") == "true") {
            return fail(ErrorCode::Unsupported, "model.ignore_merges is not supported");
        }
        rank = 0; // every merge came before the vocabulary: ranks are their positions
        for (const auto& [left, right] : pendingMerges) {
            addMerge(left, right);
        }
        out.m_byteIds.fill(-1);
        if (setting("model.byte_fallback") == "true") {
            for (int b = 0; b < 256; ++b) {
                char name[8];
                std::snprintf(name, sizeof name, "<0x%02X>", b);
                if (const auto it = out.m_vocab.find(name); it != out.m_vocab.end()) {
                    out.m_byteIds[static_cast<std::size_t>(b)] = it->second;
                }
            }
        }
        if (const auto it = out.m_vocab.find(setting("model.unk_token")); it != out.m_vocab.end()) {
            out.m_unk = it->second;
        }
        const auto eos = out.m_vocab.find("<eos>");
        const auto pad = out.m_vocab.find("<pad>");
        if (eos == out.m_vocab.end() || pad == out.m_vocab.end()) {
            return fail(ErrorCode::ParseError, "the vocabulary has no <eos> or <pad>");
        }
        out.m_eos = eos->second;
        out.m_pad = pad->second;
        return {};
    }
};

namespace {

template <class Input>
std::expected<BpeTokenizer, Error> parse(Input&& input, BpeTokenizer tokenizer) {
    BpeTokenizer::Builder builder(tokenizer);
    bool ok = false;
    try {
        ok = Json::sax_parse(std::forward<Input>(input), &builder);
    } catch (const std::exception& e) {
        return fail(ErrorCode::ParseError, e.what());
    }
    if (!ok) {
        return fail(ErrorCode::ParseError, builder.error.empty() ? "not a tokenizer.json" : builder.error);
    }
    if (auto done = builder.finish(); !done) {
        return std::unexpected(done.error());
    }
    return tokenizer;
}

} // namespace

std::expected<BpeTokenizer, Error> BpeTokenizer::load(const std::filesystem::path& file) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) {
        return fail(ErrorCode::NotFound, "no file " + pathToUtf8(file));
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return fail(ErrorCode::IoError, "cannot read " + pathToUtf8(file));
    }
    return parse(in, BpeTokenizer{});
}

std::expected<BpeTokenizer, Error> BpeTokenizer::fromJson(std::string_view json) {
    return parse(json, BpeTokenizer{});
}

// --- Encoding -----------------------------------------------------------------------------------------------------

std::vector<std::int64_t> BpeTokenizer::encode(std::string_view text, const TokenizerOptions& options) const {
    if (text.size() > kMaxTextBytes) {
        std::size_t cut = kMaxTextBytes;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
            --cut;
        }
        text = text.substr(0, cut);
    }
    const std::string lowered = options.lowercase ? lowercaseUtf8(text) : std::string(text);
    std::string normalized;
    normalized.reserve(lowered.size() * 2);
    for (const char c : lowered) {
        if (c == ' ') {
            normalized += kSpace;
        } else {
            normalized += c;
        }
    }

    // Code points (or, without a vocabulary entry, their bytes) as the initial symbols.
    std::vector<std::int32_t> symbols;
    for (std::size_t i = 0; i < normalized.size();) {
        std::size_t n = utf8Length(normalized, i);
        if (n == 0) {
            n = 1;
        }
        const auto piece = normalized.substr(i, n);
        if (const auto it = m_vocab.find(piece); it != m_vocab.end()) {
            symbols.push_back(it->second);
        } else {
            for (const char byte : piece) {
                const std::int32_t id = m_byteIds[static_cast<unsigned char>(byte)];
                const std::int32_t symbol = id >= 0 ? id : m_unk;
                // fuse_unk: consecutive unknowns are one <unk>
                if (symbol >= 0 && !(symbol == m_unk && !symbols.empty() && symbols.back() == m_unk)) {
                    symbols.push_back(symbol);
                }
            }
        }
        i += n;
    }

    // BPE: the lowest-ranked adjacent pair first, the leftmost of equal ranks (the tokenizers library's order).
    while (symbols.size() >= 2) {
        std::int32_t bestRank = INT32_MAX;
        std::size_t bestAt = 0;
        std::int32_t merged = -1;
        for (std::size_t i = 0; i + 1 < symbols.size(); ++i) {
            const auto it = m_merges.find(pairKey(symbols[i], symbols[i + 1]));
            if (it != m_merges.end() && it->second.first < bestRank) {
                bestRank = it->second.first;
                bestAt = i;
                merged = it->second.second;
            }
        }
        if (merged < 0) {
            break;
        }
        symbols[bestAt] = merged;
        symbols.erase(symbols.begin() + static_cast<std::ptrdiff_t>(bestAt) + 1);
    }

    std::vector<std::int64_t> ids(symbols.begin(), symbols.end());
    if (options.maxLength > 0 && ids.size() + 1 > options.maxLength) {
        ids.resize(options.maxLength - 1);
    }
    ids.push_back(m_eos);
    if (options.pad && options.maxLength > 0) {
        ids.resize(options.maxLength, m_pad);
    }
    return ids;
}

} // namespace wabcut::ai::search
