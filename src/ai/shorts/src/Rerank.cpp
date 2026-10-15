#include <wabcut/ai/shorts/Rerank.h>

#include <algorithm>
#include <cmath>

namespace wabcut::ai::shorts {

namespace {

/// The text cut to at most `limit` bytes at a code point.
std::string truncated(const std::string& text, std::size_t limit) {
    if (text.size() <= limit) {
        return text;
    }
    std::size_t cut = limit;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return text.substr(0, cut) + "…";
}

/// A title as the model wrote it, on one line and bounded.
std::string cleanTitle(std::string title) {
    std::ranges::replace(title, '\n', ' ');
    std::ranges::replace(title, '\r', ' ');
    std::erase(title, '"');
    while (!title.empty() && title.back() == ' ') {
        title.pop_back();
    }
    return truncated(title, 90);
}

double blended(const ShortCandidate& s, double weight) {
    return s.cloudScore ? (1.0 - weight) * s.score + weight * *s.cloudScore : s.score;
}

} // namespace

ChatRequest rerankRequest(std::span<const ShortCandidate> shorts, const RerankOptions& options) {
    ChatRequest request;
    request.system =
        "You are an experienced editor of vertical short videos (TikTok, Reels, Shorts). You get excerpts of one long "
        "recording's transcript, each a candidate short with its duration. Score each from 0 to 10 for how well it works "
        "as a standalone short: a strong hook in the first seconds, a complete thought with a payoff, clarity without the "
        "rest of the recording, and energy. Write a punchy title of at most 8 words in the transcript's language, and one "
        "short reason. Answer with JSON only.";
    std::string prompt;
    if (!options.query.empty()) {
        prompt += "The shorts should be about: " + options.query + "\n";
    }
    if (!options.language.empty()) {
        prompt += "Transcript language: " + options.language + "\n";
    }
    prompt += "Candidates:\n";
    const std::size_t n = std::min(shorts.size(), options.maxCandidates);
    for (std::size_t i = 0; i < n; ++i) {
        char head[64];
        std::snprintf(head, sizeof head, "\n[%zu] %.0f s\n", i + 1, shorts[i].range.duration.toSeconds());
        prompt += head + truncated(shorts[i].text, options.maxCharsPerShort) + "\n";
    }
    request.messages.push_back(ChatMessage::user(std::move(prompt)));
    ResponseFormat format;
    format.name = "shorts";
    format.schema = Json{
        {"type", "object"},
        {"properties",
         Json{{"shorts",
               Json{{"type", "array"},
                    {"items", Json{{"type", "object"},
                                   {"properties", Json{{"id", Json{{"type", "integer"}}},
                                                       {"score", Json{{"type", "number"}}},
                                                       {"title", Json{{"type", "string"}}},
                                                       {"why", Json{{"type", "string"}}}}},
                                   {"required", Json::array({"id", "score", "title", "why"})},
                                   {"additionalProperties", false}}}}}}},
        {"required", Json::array({"shorts"})},
        {"additionalProperties", false}};
    request.responseFormat = std::move(format);
    request.maxOutputTokens = options.maxOutputTokens;
    return request;
}

std::expected<RerankOutcome, AiError> rerankShorts(IChatModel& model, std::vector<ShortCandidate>& shorts, const RerankOptions& options,
                                                   std::stop_token stop) {
    RerankOutcome outcome;
    outcome.model = model.spec().id;
    if (shorts.empty()) {
        return outcome;
    }
    const ChatRequest request = rerankRequest(shorts, options);
    auto response = model.chat(request, {}, stop);
    if (!response) {
        return std::unexpected(response.error());
    }
    std::string text = response->message.text();
    // Tolerate a code fence around the JSON.
    if (const auto open = text.find('{'); open != std::string::npos) {
        text = text.substr(open, text.rfind('}') == std::string::npos ? std::string::npos : text.rfind('}') - open + 1);
    }
    const Json answer = Json::parse(text, nullptr, false);
    const Json* list = answer.is_object() && answer.contains("shorts") && answer["shorts"].is_array() ? &answer["shorts"] : nullptr;
    if (!list) {
        return std::unexpected(AiError(AiErrorCode::ProviderError, "the model's answer is not the requested JSON"));
    }
    const std::size_t sent = std::min(shorts.size(), options.maxCandidates);
    std::vector<ShortCandidate> updated = shorts;
    for (const Json& item : *list) {
        if (!item.is_object() || !item.contains("id") || !item["id"].is_number_integer() || !item.contains("score") || !item["score"].is_number()) {
            continue;
        }
        const auto id = item["id"].get<std::int64_t>();
        if (id < 1 || id > static_cast<std::int64_t>(sent)) {
            continue;
        }
        ShortCandidate& s = updated[static_cast<std::size_t>(id - 1)];
        const double score = item["score"].get<double>();
        if (!std::isfinite(score)) {
            continue;
        }
        s.cloudScore = std::clamp(score / 10.0, 0.0, 1.0);
        if (item.contains("title") && item["title"].is_string() && !item["title"].get<std::string>().empty()) {
            s.title = cleanTitle(item["title"].get<std::string>());
        }
        if (item.contains("why") && item["why"].is_string()) {
            s.cloudReason = truncated(item["why"].get<std::string>(), 200);
        }
        ++outcome.scored;
    }
    if (outcome.scored == 0) {
        return std::unexpected(AiError(AiErrorCode::ProviderError, "the model scored none of the shorts"));
    }
    std::ranges::stable_sort(updated, [&](const ShortCandidate& a, const ShortCandidate& b) {
        return blended(a, options.weight) > blended(b, options.weight);
    });
    for (ShortCandidate& s : updated) {
        s.score = blended(s, options.weight);
    }
    shorts = std::move(updated);
    return outcome;
}

} // namespace wabcut::ai::shorts
