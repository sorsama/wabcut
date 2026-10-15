#include <wabcut/ai/roughcut/Outline.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <tuple>

namespace wabcut::ai::roughcut {

namespace {

std::string truncated(std::string_view text, std::size_t limit) {
    if (text.size() <= limit) {
        return std::string(text);
    }
    std::size_t cut = limit;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return std::string(text.substr(0, cut)) + "\xE2\x80\xA6";
}

std::string oneLine(std::string text) {
    std::ranges::replace(text, '\n', ' ');
    std::ranges::replace(text, '\r', ' ');
    std::ranges::replace(text, '\t', ' ');
    return text;
}

} // namespace

ChatRequest outlineRequest(const Script& outline, const SpeechCorpus& corpus, const OutlineOptions& options,
                           std::vector<WordSpan>* sentences) {
    ChatRequest request;
    request.system =
        "You are a video editor's assistant. You get the outline of a video (numbered bullets) and numbered sentences from "
        "the transcripts of the recorded footage. For every bullet, in order, choose the transcript sentences that say it "
        "(copy their words exactly into \"say\", one beat per sentence, in the order they should be heard). If no sentence "
        "says a bullet, write what the speaker would most likely have said, briefly. Never invent facts. Answer with JSON "
        "only.";
    std::vector<std::string> bullets;
    for (const ScriptBeat& beat : outline.beats) {
        bullets.push_back(beat.text);
    }
    const std::vector<std::vector<WordSpan>> closest = closestSentences(corpus, bullets, options.sentencesPerBullet);
    // Number every distinct sentence once, in the order the bullets first need them.
    std::vector<WordSpan> numbered;
    std::map<std::tuple<std::size_t, std::size_t, std::size_t>, std::size_t> numberOf;
    std::string transcript;
    std::size_t chars = 0;
    for (const auto& list : closest) {
        for (const WordSpan& span : list) {
            const auto key = std::tuple{span.source, span.first, span.last};
            if (numberOf.contains(key)) {
                continue;
            }
            std::string text = oneLine(spanText(corpus, span));
            if (chars + text.size() > options.maxChars) {
                continue;
            }
            chars += text.size();
            numbered.push_back(span);
            numberOf.emplace(key, numbered.size());
            transcript += "S" + std::to_string(numbered.size()) + ": " + truncated(text, 600) + "\n";
        }
    }
    std::string prompt = "Outline";
    if (!outline.title.empty()) {
        prompt += " of \"" + truncated(oneLine(outline.title), 200) + "\"";
    }
    prompt += ":\n";
    for (std::size_t i = 0; i < outline.beats.size(); ++i) {
        prompt += std::to_string(i + 1) + ". " + truncated(oneLine(outline.beats[i].text), 400) + "\n";
    }
    prompt += "\nTranscript sentences:\n" + transcript;
    request.messages.push_back(ChatMessage::user(std::move(prompt)));
    ResponseFormat format;
    format.name = "outline_beats";
    format.schema = Json{
        {"type", "object"},
        {"properties",
         Json{{"beats", Json{{"type", "array"},
                             {"items", Json{{"type", "object"},
                                            {"properties", Json{{"bullet", Json{{"type", "integer"}}}, {"say", Json{{"type", "string"}}}}},
                                            {"required", Json::array({"bullet", "say"})},
                                            {"additionalProperties", false}}}}}}},
        {"required", Json::array({"beats"})},
        {"additionalProperties", false}};
    request.responseFormat = std::move(format);
    request.maxOutputTokens = options.maxOutputTokens;
    if (sentences) {
        *sentences = std::move(numbered);
    }
    return request;
}

std::expected<OutlineOutcome, AiError> interpretOutline(IChatModel& model, const Script& outline, const SpeechCorpus& corpus,
                                                        const OutlineOptions& options, std::stop_token stop) {
    OutlineOutcome outcome;
    outcome.model = model.spec().id;
    outcome.script.title = outline.title;
    outcome.script.language = outline.language;
    if (outline.beats.empty()) {
        return outcome;
    }
    const ChatRequest request = outlineRequest(outline, corpus, options);
    for (const ChatMessage& m : request.messages) {
        outcome.charsSent += m.text().size();
    }
    auto response = model.chat(request, {}, stop);
    if (!response) {
        return std::unexpected(response.error());
    }
    std::string text = response->message.text();
    if (const auto open = text.find('{'); open != std::string::npos) {
        const auto close = text.rfind('}');
        text = text.substr(open, close == std::string::npos || close < open ? std::string::npos : close - open + 1);
    }
    const Json answer = Json::parse(text, nullptr, false);
    const Json* list = answer.is_object() && answer.contains("beats") && answer["beats"].is_array() ? &answer["beats"] : nullptr;
    if (!list) {
        return std::unexpected(AiError(AiErrorCode::ProviderError, "the model's answer is not the requested JSON"));
    }
    std::vector<std::vector<std::string>> says(outline.beats.size());
    std::size_t accepted = 0;
    for (const Json& item : *list) {
        if (accepted >= options.maxBeats) {
            break;
        }
        if (!item.is_object() || !item.contains("bullet") || !item["bullet"].is_number_integer() || !item.contains("say") ||
            !item["say"].is_string()) {
            continue;
        }
        const auto bullet = item["bullet"].get<std::int64_t>();
        std::string say = oneLine(item["say"].get<std::string>());
        if (bullet < 1 || bullet > static_cast<std::int64_t>(outline.beats.size()) || units(say).empty()) {
            continue;
        }
        says[static_cast<std::size_t>(bullet - 1)].push_back(truncated(say, 1000));
        ++accepted;
    }
    if (accepted == 0) {
        return std::unexpected(AiError(AiErrorCode::ProviderError, "the model placed none of the bullets"));
    }
    for (std::size_t b = 0; b < outline.beats.size(); ++b) {
        const ScriptBeat& bullet = outline.beats[b];
        if (says[b].empty()) {
            ScriptBeat kept = bullet;
            kept.index = outcome.script.beats.size();
            outcome.script.beats.push_back(std::move(kept));
            continue;
        }
        ++outcome.bullets;
        for (std::size_t k = 0; k < says[b].size(); ++k) {
            ScriptBeat beat;
            beat.index = outcome.script.beats.size();
            beat.text = says[b][k];
            beat.section = bullet.section;
            beat.speaker = bullet.speaker;
            beat.outline = false; // concrete words now
            beat.line = bullet.line;
            if (k == 0) {
                beat.broll = bullet.broll;
            }
            outcome.script.beats.push_back(std::move(beat));
            ++outcome.beats;
        }
    }
    if (outcome.bullets < outline.beats.size()) {
        outcome.script.warnings.push_back(std::to_string(outline.beats.size() - outcome.bullets) +
                                          " bullets were not placed by the model and are matched as written");
    }
    return outcome;
}

} // namespace wabcut::ai::roughcut
