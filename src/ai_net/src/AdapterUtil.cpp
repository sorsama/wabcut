#include "AdapterUtil.h"

#include <wabcut/ai/JsonAccess.h>

#include <wabcut/ai/RequestLog.h>
#include <wabcut/ai/Secrets.h>
#include <wabcut/ai/Text.h>
#include <wabcut/ai/ToolSchema.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>

namespace wabcut::ai::net {

std::optional<std::vector<std::uint8_t>> base64Decode(std::string_view text) {
    std::vector<std::uint8_t> out;
    out.reserve(text.size() / 4 * 3);
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const char c : text) {
        int v = -1;
        if (c >= 'A' && c <= 'Z') {
            v = c - 'A';
        } else if (c >= 'a' && c <= 'z') {
            v = c - 'a' + 26;
        } else if (c >= '0' && c <= '9') {
            v = c - '0' + 52;
        } else if (c == '+' || c == '-') {
            v = 62;
        } else if (c == '/' || c == '_') {
            v = 63;
        } else if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t') {
            continue;
        } else {
            return std::nullopt;
        }
        buffer = (buffer << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((buffer >> bits) & 0xFF));
        }
    }
    return out;
}

std::string base64Encode(std::span<const std::uint8_t> bytes) {
    static constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const std::uint32_t v = (std::uint32_t(bytes[i]) << 16) | (std::uint32_t(bytes[i + 1]) << 8) | bytes[i + 2];
        out += kTable[(v >> 18) & 63];
        out += kTable[(v >> 12) & 63];
        out += kTable[(v >> 6) & 63];
        out += kTable[v & 63];
    }
    if (i < bytes.size()) {
        std::uint32_t v = std::uint32_t(bytes[i]) << 16;
        if (i + 1 < bytes.size()) {
            v |= std::uint32_t(bytes[i + 1]) << 8;
        }
        out += kTable[(v >> 18) & 63];
        out += kTable[(v >> 12) & 63];
        out += i + 1 < bytes.size() ? kTable[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

std::string dataUrl(const ImagePart& image) { return "data:" + image.mediaType + ";base64," + base64Encode(image.bytes); }

std::optional<Ticks> ticksFromSeconds(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0 || seconds > 1.0e7) {
        return std::nullopt;
    }
    const auto micros = static_cast<std::int64_t>(std::llround(seconds * 1.0e6));
    const auto ticks = checkedMul(micros, kTicksPerSecond / 1'000'000); // exact: 254016 ticks per microsecond
    return ticks ? std::optional<Ticks>(Ticks{*ticks}) : std::nullopt;
}

std::optional<Ticks> ticksFromMilliseconds(std::int64_t milliseconds) {
    if (milliseconds < 0) {
        return std::nullopt;
    }
    const auto ticks = checkedMul(milliseconds, kTicksPerSecond / 1000);
    return ticks ? std::optional<Ticks>(Ticks{*ticks}) : std::nullopt;
}

std::string secondsString(Ticks t) {
    const std::int64_t micros = t.value / (kTicksPerSecond / 1'000'000);
    char buf[48];
    std::snprintf(buf, sizeof buf, "%lld.%06llds", static_cast<long long>(micros / 1'000'000),
                  static_cast<long long>(micros % 1'000'000));
    std::string s(buf);
    // "12.500000s" -> "12.5s", "3.000000s" -> "3s"
    const auto dot = s.find('.');
    std::size_t end = s.size() - 1; // before 's'
    while (end > dot + 1 && s[end - 1] == '0') {
        --end;
    }
    if (end == dot + 1) {
        end = dot;
    }
    return s.substr(0, end) + "s";
}

std::string normalizeLanguage(std::string_view code) {
    std::string lower(code);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    const auto sep = lower.find_first_of("-_");
    if (sep != std::string::npos) {
        lower.resize(sep);
    }
    if (lower.size() == 3) {
        static const std::map<std::string, std::string, std::less<>> k639_3{
            {"ara", "ar"}, {"ben", "bn"}, {"ces", "cs"}, {"cmn", "zh"}, {"dan", "da"}, {"deu", "de"}, {"ell", "el"},
            {"eng", "en"}, {"fin", "fi"}, {"fra", "fr"}, {"heb", "he"}, {"hin", "hi"}, {"hun", "hu"}, {"ind", "id"},
            {"ita", "it"}, {"jpn", "ja"}, {"kor", "ko"}, {"msa", "ms"}, {"nld", "nl"}, {"nor", "no"}, {"pol", "pl"},
            {"por", "pt"}, {"ron", "ro"}, {"rus", "ru"}, {"spa", "es"}, {"swe", "sv"}, {"tha", "th"}, {"tur", "tr"},
            {"ukr", "uk"}, {"vie", "vi"}, {"yue", "zh"}, {"zho", "zh"}, {"fil", "tl"}, {"tgl", "tl"}, {"lao", "lo"},
            {"khm", "km"}, {"mya", "my"},
        };
        if (auto it = k639_3.find(lower); it != k639_3.end()) {
            return it->second;
        }
    }
    return lower;
}

std::string endpointUrl(const ProviderContext& context) { return joinUrl(context.baseUrl, context.model.endpoint); }

HttpHeaders providerHeaders(const ProviderContext& context) {
    HttpHeaders headers;
    applyAuth(headers, context.provider.auth, context.key.reveal());
    for (const auto& h : context.provider.extraHeaders) {
        headers.push_back(h);
    }
    return headers;
}

HttpRequest jsonRequest(const ProviderContext& context, std::string url, const Json& body, BodyMode mode) {
    HttpRequest request;
    request.method = "POST";
    request.url = std::move(url);
    request.headers = providerHeaders(context);
    request.headers.push_back({"content-type", "application/json"});
    if (mode == BodyMode::Sse) {
        request.headers.push_back({"accept", "text/event-stream"});
    }
    request.body = body.dump(-1, ' ', false, Json::error_handler_t::replace);
    request.responseMode = mode;
    return request;
}

AiError httpError(const HttpResponse& response) {
    return classifyHttpError(response.status, response.headers, response.body);
}

std::expected<Json, AiError> parseJsonBody(std::string_view body, std::string_view what) {
    auto j = Json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        return std::unexpected(AiError(AiErrorCode::ProviderError, "unreadable " + std::string(what) + " response"));
    }
    return j;
}

AiError streamError(std::string_view providerCode, std::string_view message) {
    std::string probe(providerCode);
    std::transform(probe.begin(), probe.end(), probe.begin(), [](unsigned char c) { return std::tolower(c); });
    int status = 500;
    if (probe.find("overloaded") != std::string::npos) {
        status = 529;
    } else if (probe.find("rate_limit") != std::string::npos || probe.find("too_many") != std::string::npos ||
               probe.find("resource_exhausted") != std::string::npos) {
        status = 429;
    } else if (probe.find("quota") != std::string::npos || probe.find("billing") != std::string::npos) {
        status = 402;
    } else if (probe.find("invalid") != std::string::npos || probe.find("malformed") != std::string::npos ||
               probe.find("missing") != std::string::npos) {
        status = 400;
    } else if (probe.find("auth") != std::string::npos || probe.find("permission") != std::string::npos) {
        status = 401;
    }
    const Json body = {{"error", {{"code", std::string(providerCode)}, {"message", std::string(message)}}}};
    AiError e = classifyHttpError(status, {}, body.dump());
    e.httpStatus = 200; // the HTTP exchange itself succeeded
    e.message = "in stream: " + e.message;
    return e;
}

std::expected<void, AiError> rejectVideo(const ChatRequest& request, std::string_view api) {
    for (const auto& message : request.messages) {
        for (const auto& part : message.parts) {
            if (std::holds_alternative<VideoPart>(part)) {
                return std::unexpected(AiError(AiErrorCode::Unsupported,
                                               "the " + std::string(api) + " API takes no video; send sampled frames"));
            }
        }
    }
    return {};
}

std::int64_t intAt(const Json& j, std::initializer_list<const char*> path) {
    const Json* cur = &j;
    for (const char* key : path) {
        if (!cur->is_object()) {
            return 0;
        }
        auto it = cur->find(key);
        if (it == cur->end()) {
            return 0;
        }
        cur = &*it;
    }
    return asInt(cur).value_or(0);
}

std::string stringAt(const Json& j, std::initializer_list<const char*> path) {
    const Json* cur = &j;
    for (const char* key : path) {
        if (!cur->is_object()) {
            return {};
        }
        auto it = cur->find(key);
        if (it == cur->end()) {
            return {};
        }
        cur = &*it;
    }
    return cur->is_string() ? cur->get<std::string>() : std::string();
}

// --- ChatAccumulator --------------------------------------------------------------------------------------------

namespace {

std::string mib(std::size_t bytes) { return std::to_string(bytes / (1024 * 1024)) + " MiB"; }

AiError malformed(std::string_view what) { return malformedEvent(what); }

} // namespace

AiError malformedEvent(std::string_view what) {
    return AiError(AiErrorCode::ProviderError, "malformed provider event: " + std::string(what));
}

bool ChatAccumulator::newPart() {
    if (failed()) {
        return false;
    }
    if (m_response.message.parts.size() >= kMaxResponseParts) {
        fail(AiError(AiErrorCode::PayloadTooLarge,
                     "the response has more than " + std::to_string(kMaxResponseParts) + " parts; it was cut off"));
        return false;
    }
    m_response.message.role = Role::Assistant;
    return true;
}

bool ChatAccumulator::addText(std::size_t bytes) {
    if (m_textBytes + bytes > kMaxResponseText) {
        fail(AiError(AiErrorCode::PayloadTooLarge, "the response text is longer than " + mib(kMaxResponseText) +
                                                       "; it was cut off"));
        return false;
    }
    m_textBytes += bytes;
    return true;
}

void ChatAccumulator::fail(AiError error) {
    if (!m_error) {
        m_error = std::move(error);
    }
}

std::size_t ChatAccumulator::startText() {
    if (!newPart()) {
        return kNoPart;
    }
    m_response.message.parts.emplace_back(TextPart{});
    return m_response.message.parts.size() - 1;
}

void ChatAccumulator::appendText(std::size_t part, std::string_view text) {
    if (text.empty() || failed()) {
        return;
    }
    auto* t = part < m_response.message.parts.size() ? std::get_if<TextPart>(&m_response.message.parts[part]) : nullptr;
    if (!t) {
        fail(malformed("text for a part that is not text"));
        return;
    }
    if (!addText(text.size())) {
        return;
    }
    t->text.append(text);
    if (m_onEvent) {
        m_onEvent(TextDelta{part, std::string(text)});
    }
}

std::size_t ChatAccumulator::startToolCall(std::string id, std::string name) {
    if (!newPart()) {
        return kNoPart;
    }
    ToolCall call;
    call.id = std::move(id);
    call.name = std::move(name);
    m_response.message.parts.emplace_back(call);
    const std::size_t part = m_response.message.parts.size() - 1;
    if (m_onEvent) {
        m_onEvent(ToolCallStarted{part, call.id, call.name});
    }
    return part;
}

ToolCall* ChatAccumulator::toolCall(std::size_t part) {
    return part < m_response.message.parts.size() ? std::get_if<ToolCall>(&m_response.message.parts[part]) : nullptr;
}

void ChatAccumulator::appendArguments(std::size_t part, std::string_view text) {
    if (text.empty() || failed()) {
        return;
    }
    ToolCall* call = toolCall(part);
    if (!call) {
        fail(malformed("arguments for a part that is not a tool call"));
        return;
    }
    if (call->argumentsText.size() + text.size() > kMaxToolArguments) {
        fail(AiError(AiErrorCode::PayloadTooLarge, "the arguments of tool call \"" + call->name +
                                                       "\" are longer than " + mib(kMaxToolArguments) +
                                                       "; the response was cut off"));
        return;
    }
    call->argumentsText.append(text);
    if (m_onEvent) {
        m_onEvent(ToolCallArgumentsDelta{part, std::string(text)});
    }
}

void ChatAccumulator::setArguments(std::size_t part, std::string text) {
    if (failed()) {
        return;
    }
    ToolCall* call = toolCall(part);
    if (!call) {
        fail(malformed("arguments for a part that is not a tool call"));
        return;
    }
    if (text.size() > kMaxToolArguments) {
        fail(AiError(AiErrorCode::PayloadTooLarge, "the arguments of tool call \"" + call->name +
                                                       "\" are longer than " + mib(kMaxToolArguments) +
                                                       "; the response was cut off"));
        return;
    }
    call->argumentsText = std::move(text);
}

void ChatAccumulator::finishToolCall(std::size_t part) {
    ToolCall* call = toolCall(part);
    if (!call) {
        return;
    }
    auto args = parseToolArguments(call->argumentsText);
    call->arguments = args ? std::move(*args) : Json(nullptr);
    if (call->argumentsText.empty()) {
        call->argumentsText = "{}";
    }
}

std::size_t ChatAccumulator::startReasoning(ReasoningPart part) {
    if (!newPart() || !addText(part.text.size() + part.signature.size())) {
        return kNoPart;
    }
    m_response.message.parts.emplace_back(std::move(part));
    return m_response.message.parts.size() - 1;
}

void ChatAccumulator::appendSignature(std::size_t part, std::string_view signature) {
    if (signature.empty() || failed()) {
        return;
    }
    ReasoningPart* r = reasoning(part);
    if (!r) {
        fail(malformed("a signature for a part that is not reasoning"));
        return;
    }
    if (addText(signature.size())) {
        r->signature.append(signature);
    }
}

void ChatAccumulator::setSignature(std::size_t part, std::string signature) {
    if (failed()) {
        return;
    }
    ReasoningPart* r = reasoning(part);
    if (!r) {
        fail(malformed("a signature for a part that is not reasoning"));
        return;
    }
    if (addText(signature.size())) {
        r->signature = std::move(signature);
    }
}

ReasoningPart* ChatAccumulator::reasoning(std::size_t part) {
    return part < m_response.message.parts.size() ? std::get_if<ReasoningPart>(&m_response.message.parts[part]) : nullptr;
}

void ChatAccumulator::appendReasoning(std::size_t part, std::string_view text) {
    if (text.empty() || failed()) {
        return;
    }
    ReasoningPart* r = reasoning(part);
    if (!r) {
        fail(malformed("reasoning for a part that is not reasoning"));
        return;
    }
    if (!addText(text.size())) {
        return;
    }
    r->text.append(text);
    if (m_onEvent) {
        m_onEvent(ReasoningDelta{part, std::string(text)});
    }
}

void ChatAccumulator::updateUsage(const Usage& usage) {
    m_response.usage = usage;
    if (m_onEvent) {
        m_onEvent(UsageUpdate{usage});
    }
}

bool ChatAccumulator::hasToolCalls() const {
    return std::any_of(m_response.message.parts.begin(), m_response.message.parts.end(),
                       [](const ContentPart& p) { return std::holds_alternative<ToolCall>(p); });
}

ChatResponse ChatAccumulator::take() {
    m_response.message.role = Role::Assistant;
    for (std::size_t i = 0; i < m_response.message.parts.size(); ++i) {
        if (auto* c = std::get_if<ToolCall>(&m_response.message.parts[i]); c && c->arguments.is_null()) {
            finishToolCall(i);
        }
    }
    std::erase_if(m_response.message.parts, [](const ContentPart& p) {
        const auto* t = std::get_if<TextPart>(&p);
        return t && t->text.empty();
    });
    return std::move(m_response);
}

// --- TranscriptBuilder ------------------------------------------------------------------------------------------

TranscriptBuilder::TranscriptBuilder(const TranscribeRequest& request) : m_request(request) {
    m_transcript.source = request.source;
    m_transcript.language = normalizeLanguage(request.language);
}

std::optional<int> TranscriptBuilder::speakerId(std::string_view label) {
    if (label.empty()) {
        return std::nullopt;
    }
    auto it = std::find(m_labels.begin(), m_labels.end(), label);
    if (it == m_labels.end()) {
        m_labels.emplace_back(label);
        return static_cast<int>(m_labels.size() - 1);
    }
    return static_cast<int>(it - m_labels.begin());
}

void TranscriptBuilder::addWord(std::string text, Ticks t0, Ticks t1, float confidence, std::string_view speaker) {
    TranscriptWord w;
    w.text = std::string(text::trimAsciiSpace(text::sanitizeUtf8(text)));
    if (w.text.empty()) {
        return;
    }
    w.t0 = m_request.origin + t0;
    w.t1 = m_request.origin + std::max(t0, t1);
    w.confidence = std::isfinite(confidence) ? std::clamp(confidence, 0.0f, 1.0f) : 0.0f;
    w.speakerId = speakerId(speaker);
    m_transcript.words.push_back(std::move(w));
}

void TranscriptBuilder::addUtterance(Ticks t0, Ticks t1) {
    m_utterances.emplace_back(m_request.origin + t0, m_request.origin + std::max(t0, t1));
}

void TranscriptBuilder::setLanguage(std::string_view language) {
    if (!language.empty()) {
        m_transcript.language = normalizeLanguage(language);
    }
}

std::expected<Transcript, AiError> TranscriptBuilder::build(Provenance provenance) {
    auto& words = m_transcript.words;
    std::stable_sort(words.begin(), words.end(), [](const TranscriptWord& a, const TranscriptWord& b) { return a.t0 < b.t0; });

    auto makeSegment = [&](std::size_t first, std::size_t count) {
        TranscriptSegment s;
        s.firstWord = first;
        s.wordCount = count;
        s.t0 = words[first].t0;
        s.t1 = words[first].t1;
        std::map<int, std::size_t> votes;
        for (std::size_t i = first; i < first + count; ++i) {
            s.t1 = std::max(s.t1, words[i].t1);
            if (!s.text.empty()) {
                s.text += ' ';
            }
            s.text += words[i].text;
            if (words[i].speakerId) {
                ++votes[*words[i].speakerId];
            }
        }
        if (!votes.empty()) {
            s.speakerId = std::max_element(votes.begin(), votes.end(), [](const auto& a, const auto& b) {
                              return a.second < b.second;
                          })->first;
        }
        m_transcript.segments.push_back(std::move(s));
    };

    std::size_t first = 0;
    if (!m_utterances.empty()) {
        std::sort(m_utterances.begin(), m_utterances.end());
        // Each word joins the last utterance that starts at or before it.
        std::size_t u = 0;
        for (std::size_t i = 0; i < words.size(); ++i) {
            std::size_t target = u;
            while (target + 1 < m_utterances.size() && m_utterances[target + 1].first <= words[i].t0) {
                ++target;
            }
            if (target != u && i > first) {
                makeSegment(first, i - first);
                first = i;
            }
            u = target;
        }
    } else {
        const Ticks pause = Ticks::fromSeconds(1) + Ticks{kTicksPerSecond / 2};
        for (std::size_t i = 1; i < words.size(); ++i) {
            const bool speakerChange = words[i].speakerId != words[i - 1].speakerId;
            const bool gap = words[i].t0 - words[i - 1].t1 > pause;
            if (speakerChange || gap) {
                makeSegment(first, i - first);
                first = i;
            }
        }
    }
    if (first < words.size()) {
        makeSegment(first, words.size() - first);
    }
    m_transcript.provenance = std::move(provenance);
    if (auto valid = m_transcript.validate(); !valid) {
        return std::unexpected(AiError(AiErrorCode::ProviderError, "inconsistent transcript: " + valid.error().message));
    }
    return std::move(m_transcript);
}

Provenance cloudProvenance(const ProviderContext& context, const TranscribeRequest& request, std::string modelUsed,
                           std::string modelVersion) {
    Provenance p;
    p.engine = context.provider.id;
    p.engineVersion = context.model.api;
    p.modelId = modelUsed.empty() ? context.model.id : std::move(modelUsed);
    p.modelVersion = std::move(modelVersion);
    p.params["language"] = request.language.empty() ? "auto" : request.language;
    p.params["diarize"] = request.diarize ? "true" : "false";
    p.createdAt = nowIso8601();
    return p;
}

} // namespace wabcut::ai::net
