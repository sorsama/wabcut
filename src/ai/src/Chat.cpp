#include <wabcut/ai/Chat.h>

namespace wabcut::ai {

ChatMessage ChatMessage::user(std::string text) {
    ChatMessage m;
    m.role = Role::User;
    m.parts.emplace_back(TextPart{std::move(text)});
    return m;
}

ChatMessage ChatMessage::assistant(std::string text) {
    ChatMessage m;
    m.role = Role::Assistant;
    m.parts.emplace_back(TextPart{std::move(text)});
    return m;
}

ChatMessage ChatMessage::toolResults(std::vector<ToolResult> results) {
    ChatMessage m;
    m.role = Role::User;
    for (auto& r : results) {
        m.parts.emplace_back(std::move(r));
    }
    return m;
}

std::string ChatMessage::text() const {
    std::string out;
    for (const auto& part : parts) {
        if (const auto* t = std::get_if<TextPart>(&part)) {
            out += t->text;
        }
    }
    return out;
}

std::vector<ToolCall> ChatMessage::toolCalls() const {
    std::vector<ToolCall> out;
    for (const auto& part : parts) {
        if (const auto* c = std::get_if<ToolCall>(&part)) {
            out.push_back(*c);
        }
    }
    return out;
}

std::vector<ToolSpec> toolSpecsFromCommandExport(const Json& exported) {
    std::vector<ToolSpec> out;
    if (!exported.is_array()) {
        return out;
    }
    for (const auto& entry : exported) {
        if (!entry.is_object() || !entry.contains("toolName") || !entry["toolName"].is_string()) {
            continue;
        }
        ToolSpec spec;
        spec.name = entry["toolName"].get<std::string>();
        if (auto it = entry.find("description"); it != entry.end() && it->is_string()) {
            spec.description = it->get<std::string>();
        }
        if (auto it = entry.find("parameters"); it != entry.end() && it->is_object()) {
            spec.parameters = *it;
        }
        out.push_back(std::move(spec));
    }
    return out;
}

std::string_view toString(StopReason reason) {
    switch (reason) {
    case StopReason::EndTurn:
        return "end_turn";
    case StopReason::ToolUse:
        return "tool_use";
    case StopReason::MaxTokens:
        return "max_tokens";
    case StopReason::StopSequence:
        return "stop_sequence";
    case StopReason::Refusal:
        return "refusal";
    case StopReason::ContentFilter:
        return "content_filter";
    case StopReason::Other:
        return "other";
    }
    return "other";
}

Usage& Usage::operator+=(const Usage& other) {
    inputTokens += other.inputTokens;
    outputTokens += other.outputTokens;
    cacheReadTokens += other.cacheReadTokens;
    cacheWriteTokens += other.cacheWriteTokens;
    reasoningTokens += other.reasoningTokens;
    audioMilliseconds += other.audioMilliseconds;
    characters += other.characters;
    videoMilliseconds += other.videoMilliseconds;
    images += other.images;
    if (mediaTier.empty()) {
        mediaTier = other.mediaTier;
    }
    return *this;
}

} // namespace wabcut::ai
