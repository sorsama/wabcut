#include <wabcut/ai/translate/MarianTokenizer.h>

#include <wabcut/model/MediaLocator.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <limits>
#include <sstream>

namespace wabcut::ai::translate {

namespace {

constexpr std::string_view kMeta = "\xE2\x96\x81"; // U+2581, SentencePiece's word start

std::size_t charLength(unsigned char c) {
    return c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
}

/// Whitespace as tokenizers' WhitespaceSplit sees it (the common forms): ASCII space and controls, NBSP, ideographic
/// space, the en/em spaces.
std::size_t whitespaceAt(std::string_view s, std::size_t i) {
    const auto c = static_cast<unsigned char>(s[i]);
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f') {
        return 1;
    }
    if (s.substr(i, 2) == "\xC2\xA0") {
        return 2;
    }
    if (s.substr(i, 3) == "\xE3\x80\x80" || (s.substr(i, 2) == "\xE2\x80" && i + 2 < s.size() &&
                                             static_cast<unsigned char>(s[i + 2]) >= 0x80 && static_cast<unsigned char>(s[i + 2]) <= 0x8A)) {
        return 3;
    }
    return 0;
}

bool isTargetToken(std::string_view piece) { return piece.size() > 4 && piece.starts_with(">>") && piece.ends_with("<<"); }

void replaceAll(std::string& s, std::string_view from, std::string_view to) {
    for (std::size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size())) {
        s.replace(at, from.size(), to);
    }
}

// SentencePiece normalised the training text with NFKC, and the exports' tokenizer.json lost that table: the pieces
// only know what NFKC makes of the text. The cases that matter for captions are done here: Thai SARA AM and Lao AM
// (decomposed into NIKHAHIT / NIGGAHITA + AA) and full-width ASCII (to ASCII).
constexpr std::string_view kThaiAm = "\xE0\xB8\xB3";          // U+0E33
constexpr std::string_view kThaiAmParts = "\xE0\xB9\x8D\xE0\xB8\xB2"; // U+0E4D U+0E32
constexpr std::string_view kLaoAm = "\xE0\xBA\xB3";           // U+0EB3
constexpr std::string_view kLaoAmParts = "\xE0\xBB\x8D\xE0\xBA\xB2";  // U+0ECD U+0EB2

std::string normalized(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        // U+FF01..U+FF5E (EF BC 81 .. EF BD 9E) -> U+0021..U+007E
        if (i + 2 < text.size() && static_cast<unsigned char>(text[i]) == 0xEF) {
            const auto b1 = static_cast<unsigned char>(text[i + 1]);
            const auto b2 = static_cast<unsigned char>(text[i + 2]);
            const int code = ((b1 & 0x3F) << 6) | (b2 & 0x3F);
            const int full = 0xF000 | code; // with the lead byte's 0xF
            if (full >= 0xFF01 && full <= 0xFF5E) {
                out.push_back(static_cast<char>(full - 0xFF01 + 0x21));
                i += 3;
                continue;
            }
        }
        out.push_back(text[i++]);
    }
    replaceAll(out, kThaiAm, kThaiAmParts);
    replaceAll(out, kLaoAm, kLaoAmParts);
    return out;
}

} // namespace

std::expected<MarianTokenizer, Error> MarianTokenizer::load(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return std::unexpected(Error{ErrorCode::IoError, "cannot read " + pathToUtf8(file)});
    }
    std::ostringstream text;
    text << in.rdbuf();
    return fromJson(text.str());
}

std::expected<MarianTokenizer, Error> MarianTokenizer::fromJson(std::string_view json) {
    const auto bad = [](std::string message) { return std::unexpected(Error{ErrorCode::ParseError, "tokenizer.json: " + std::move(message)}); };
    nlohmann::json j = nlohmann::json::parse(json, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        return bad("not JSON");
    }
    const auto model = j.find("model");
    if (model == j.end() || !model->is_object() || model->value("type", std::string()) != "Unigram") {
        return bad("not a unigram model");
    }
    const auto vocab = model->find("vocab");
    if (vocab == model->end() || !vocab->is_array() || vocab->empty()) {
        return bad("no vocabulary");
    }
    MarianTokenizer t;
    t.m_pieces.reserve(vocab->size());
    t.m_scores.reserve(vocab->size());
    double minScore = 0.0;
    for (const auto& entry : *vocab) {
        if (!entry.is_array() || entry.size() != 2 || !entry[0].is_string() || !entry[1].is_number()) {
            return bad("a vocabulary entry is not [piece, score]");
        }
        t.m_pieces.push_back(entry[0].get<std::string>());
        t.m_scores.push_back(entry[1].get<double>());
        minScore = std::min(minScore, t.m_scores.back());
    }
    t.m_unkScore = minScore - 10.0;
    for (std::size_t i = 0; i < t.m_pieces.size(); ++i) {
        t.m_ids.emplace(t.m_pieces[i], static_cast<std::int64_t>(i));
    }
    const auto special = [&](std::string_view piece) -> std::optional<std::int64_t> {
        const auto it = t.m_ids.find(std::string(piece));
        return it != t.m_ids.end() ? std::optional(it->second) : std::nullopt;
    };
    const auto eos = special("</s>");
    const auto unk = special("<unk>");
    const auto pad = special("<pad>");
    if (!eos || !unk || !pad) {
        return bad("no </s>, <unk> or <pad>");
    }
    t.m_eos = *eos;
    t.m_unk = *unk;
    t.m_pad = *pad;
    std::vector<std::string> specials{"</s>", "<unk>", "<pad>"};
    if (const auto added = j.find("added_tokens"); added != j.end() && added->is_array()) {
        for (const auto& a : *added) {
            if (a.is_object() && a.value("special", false) && a.contains("content") && a["content"].is_string()) {
                specials.push_back(a["content"].get<std::string>());
            }
        }
    }
    for (std::size_t i = 0; i < t.m_pieces.size(); ++i) {
        const std::string& p = t.m_pieces[i];
        if (p.empty() || std::ranges::find(specials, p) != specials.end() || isTargetToken(p)) {
            continue;
        }
        t.m_split.emplace(p, static_cast<std::int64_t>(i));
        t.m_maxPieceBytes = std::max(t.m_maxPieceBytes, p.size());
    }
    return t;
}

std::optional<std::int64_t> MarianTokenizer::idOf(std::string_view piece) const {
    const auto it = m_ids.find(std::string(piece));
    return it != m_ids.end() ? std::optional(it->second) : std::nullopt;
}

const std::string& MarianTokenizer::piece(std::int64_t id) const {
    static const std::string none;
    return id >= 0 && static_cast<std::size_t>(id) < m_pieces.size() ? m_pieces[static_cast<std::size_t>(id)] : none;
}

void MarianTokenizer::encodeWord(std::string_view word, std::vector<std::int64_t>& out) const {
    // Viterbi over character boundaries: best[i] is the best score of word[0, i).
    const std::size_t n = word.size();
    constexpr double kNone = -std::numeric_limits<double>::infinity();
    std::vector<double> best(n + 1, kNone);
    std::vector<std::size_t> from(n + 1, 0);
    std::vector<std::int64_t> via(n + 1, -1);
    best[0] = 0.0;
    std::string key;
    for (std::size_t i = 0; i < n;) {
        const std::size_t step = std::min(charLength(static_cast<unsigned char>(word[i])), n - i);
        if (best[i] != kNone) {
            // Every piece starting here (at whole characters).
            for (std::size_t end = i + step; end <= n && end - i <= m_maxPieceBytes;) {
                key.assign(word.substr(i, end - i));
                if (const auto it = m_split.find(key); it != m_split.end()) {
                    const double score = best[i] + m_scores[static_cast<std::size_t>(it->second)];
                    if (score > best[end]) {
                        best[end] = score;
                        from[end] = i;
                        via[end] = it->second;
                    }
                }
                if (end >= n) {
                    break;
                }
                end += std::min(charLength(static_cast<unsigned char>(word[end])), n - end);
            }
            // The character on its own as <unk>.
            const double unknown = best[i] + m_unkScore;
            if (unknown > best[i + step]) {
                best[i + step] = unknown;
                from[i + step] = i;
                via[i + step] = m_unk;
            }
        }
        i += step;
    }
    std::vector<std::int64_t> pieces;
    for (std::size_t at = n; at > 0; at = from[at]) {
        pieces.push_back(via[at]);
    }
    std::reverse(pieces.begin(), pieces.end());
    for (const std::int64_t id : pieces) {
        if (id == m_unk && !out.empty() && out.back() == m_unk) {
            continue; // a run of unknown characters is one <unk>, as in SentencePiece
        }
        out.push_back(id);
    }
}

std::vector<std::int64_t> MarianTokenizer::encode(std::string_view raw, std::string_view targetToken) const {
    const std::string normalizedText = normalized(raw);
    const std::string_view text = normalizedText;
    std::vector<std::int64_t> out;
    if (!targetToken.empty()) {
        if (const auto id = idOf(targetToken)) {
            out.push_back(*id);
        }
    }
    std::size_t i = 0;
    while (i < text.size()) {
        if (const std::size_t ws = whitespaceAt(text, i)) {
            i += ws;
            continue;
        }
        std::size_t end = i;
        while (end < text.size() && whitespaceAt(text, end) == 0) {
            end += std::min(charLength(static_cast<unsigned char>(text[end])), text.size() - end);
        }
        encodeWord(std::string(kMeta) + std::string(text.substr(i, end - i)), out);
        i = end;
    }
    out.push_back(m_eos);
    return out;
}

std::string MarianTokenizer::decode(std::span<const std::int64_t> ids) const {
    std::string joined;
    for (const std::int64_t id : ids) {
        if (id == m_eos || id == m_pad || id == m_unk) {
            continue;
        }
        const std::string& p = piece(id);
        if (isTargetToken(p)) {
            continue;
        }
        joined += p;
    }
    std::string out;
    out.reserve(joined.size());
    for (std::size_t i = 0; i < joined.size();) {
        if (std::string_view(joined).substr(i, kMeta.size()) == kMeta) {
            if (!out.empty() && out.back() != ' ') {
                out.push_back(' ');
            }
            i += kMeta.size();
        } else {
            out.push_back(joined[i++]);
        }
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    // Back to the composed forms people type.
    replaceAll(out, kThaiAmParts, kThaiAm);
    replaceAll(out, kLaoAmParts, kLaoAm);
    return out;
}

} // namespace wabcut::ai::translate
