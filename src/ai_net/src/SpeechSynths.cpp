// Text-to-speech adapters (AI-12, docs/ai.md "Going global"), checked against the official docs on 2026-10-12:
//   ElevenLabs  POST /v1/text-to-speech/{voice_id}/with-timestamps?output_format=mp3_44100_128, xi-api-key,
//               {"text", "model_id"} -> {"audio_base64", "alignment": {"characters", "character_start_times_seconds",
//               "character_end_times_seconds"}}. Billed per character.
//               elevenlabs.io/docs/api-reference/text-to-speech/convert-with-timestamps
//   OpenAI      POST /v1/audio/speech, bearer, {"model", "input" (<= 4096 chars), "voice", "instructions",
//               "response_format": "wav"} -> the WAV file. Billed per text token and audio token.
//               developers.openai.com/api/reference/resources/audio/subresources/speech/methods/create
//   Gemini      POST /v1beta/interactions, x-goog-api-key, {"model", "input": [{"type": "user_input", "content":
//               [{"type": "text", "text", "annotations": [{"type": "speech_metadata", "style"}]}]}], "response_format":
//               {"type": "audio", "mime_type": "audio/wav"}, "generation_config": {"speech_config": [{"voice"}]}} ->
//               the audio base64 in "output_audio.data" or a model_output step's audio content. 25 audio tokens a second.
//               ai.google.dev/gemini-api/docs/generate-content/speech-generation
// None of them is retried: a network ambiguity must not bill a line twice.

#include "AdapterUtil.h"

#include <wabcut/ai/JsonAccess.h>
#include <wabcut/ai_net/Adapters.h>
#include <wabcut/model/MediaLocator.h>

#include <algorithm>
#include <cctype>
#include <cmath>

namespace wabcut::ai::net {

namespace {

std::size_t codePoints(std::string_view text) {
    std::size_t n = 0;
    for (const char c : text) {
        n += (static_cast<unsigned char>(c) & 0xC0) != 0x80 ? 1 : 0;
    }
    return n;
}

/// A WAV file's duration (RIFF, "fmt " and "data" chunks; a streamed data size of 0 or 0xFFFFFFFF reads to the end).
std::optional<std::int64_t> wavMilliseconds(std::span<const std::uint8_t> wav) {
    const auto u32 = [&](std::size_t at) {
        return static_cast<std::uint32_t>(wav[at]) | (static_cast<std::uint32_t>(wav[at + 1]) << 8) |
               (static_cast<std::uint32_t>(wav[at + 2]) << 16) | (static_cast<std::uint32_t>(wav[at + 3]) << 24);
    };
    if (wav.size() < 12 || std::string_view(reinterpret_cast<const char*>(wav.data()), 4) != "RIFF" ||
        std::string_view(reinterpret_cast<const char*>(wav.data()) + 8, 4) != "WAVE") {
        return std::nullopt;
    }
    std::uint32_t byteRate = 0;
    std::size_t at = 12;
    while (at + 8 <= wav.size()) {
        const std::string_view id(reinterpret_cast<const char*>(wav.data()) + at, 4);
        std::uint64_t size = u32(at + 4);
        if (id == "fmt " && at + 8 + 16 <= wav.size()) {
            byteRate = u32(at + 8 + 8);
        } else if (id == "data") {
            const std::uint64_t available = wav.size() - (at + 8);
            if (size == 0 || size == 0xFFFFFFFFu || size > available) {
                size = available;
            }
            if (byteRate == 0) {
                return std::nullopt;
            }
            return static_cast<std::int64_t>(size * 1000 / byteRate);
        }
        at += 8 + static_cast<std::size_t>(size) + (size & 1);
    }
    return std::nullopt;
}

std::vector<std::uint8_t> bytesOf(std::string_view body) { return {body.begin(), body.end()}; }

HttpRequest paidRequest(HttpRequest http) {
    http.retrySafe = false;
    http.timeout = std::chrono::seconds(120);
    http.idleTimeout = std::chrono::seconds(60);
    return http;
}

template <class Parse>
auto guarded(std::string_view provider, Parse&& parse) -> decltype(parse()) {
    try {
        return parse();
    } catch (const std::exception&) {
        return std::unexpected(AiError(AiErrorCode::ProviderError, std::string(provider) + " sent an answer that cannot be read"));
    }
}

class SpeechBase : public ISpeechSynth {
public:
    explicit SpeechBase(ProviderContext context)
        : m_provider(context.provider), m_spec(context.model), m_baseUrl(std::move(context.baseUrl)), m_key(std::move(context.key)),
          m_transport(context.transport) {}

    const ModelSpec& spec() const override { return m_spec; }

protected:
    ProviderContext context() const { return ProviderContext{m_provider, m_spec, m_baseUrl, m_key.clone(), m_transport}; }

    std::expected<void, AiError> checkText(const SpeechRequest& request, std::size_t limit) const {
        if (request.text.empty()) {
            return std::unexpected(AiError(AiErrorCode::InvalidRequest, "nothing to say"));
        }
        if (codePoints(request.text) > limit) {
            return std::unexpected(AiError(AiErrorCode::PayloadTooLarge, "the line is longer than the voice model takes (" +
                                                                             std::to_string(limit) + " characters)"));
        }
        if (request.voiceId.empty()) {
            return std::unexpected(AiError(AiErrorCode::InvalidRequest, "no voice chosen"));
        }
        return {};
    }

    ProviderSpec m_provider;
    ModelSpec m_spec;
    std::string m_baseUrl;
    Secret m_key;
    IHttpTransport& m_transport;
};

// --- ElevenLabs -----------------------------------------------------------------------------------------------------

class ElevenLabsSpeech final : public SpeechBase {
public:
    using SpeechBase::SpeechBase;

    std::expected<SpeechResult, AiError> synthesize(const SpeechRequest& request, std::stop_token stop) override {
        if (auto ok = checkText(request, 5000); !ok) {
            return std::unexpected(ok.error());
        }
        for (const char c : request.voiceId) {
            if (!std::isalnum(static_cast<unsigned char>(c))) {
                return std::unexpected(AiError(AiErrorCode::InvalidRequest, "not an ElevenLabs voice id"));
            }
        }
        const ProviderContext c = context();
        Json body{{"text", request.text}, {"model_id", m_spec.id}};
        HttpRequest http = paidRequest(jsonRequest(c, joinUrl(m_baseUrl, "/v1/text-to-speech/" + request.voiceId + "/with-timestamps") +
                                                          "?output_format=mp3_44100_128",
                                                  body, BodyMode::Buffered));
        auto response = m_transport.send(http, {}, stop);
        if (!response) {
            return std::unexpected(response.error());
        }
        if (!response->ok()) {
            return std::unexpected(httpError(*response));
        }
        auto parsed = parseJsonBody(response->body, "ElevenLabs");
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        const Json& j = *parsed;
        return guarded("ElevenLabs", [&]() -> std::expected<SpeechResult, AiError> {
            auto audio = base64Decode(jsonStringOr(j, "audio_base64"));
            if (!audio || audio->size() < 16) {
                return std::unexpected(AiError(AiErrorCode::ProviderError, "ElevenLabs sent no audio"));
            }
            SpeechResult result;
            result.audio = std::move(*audio);
            result.mediaType = "audio/mpeg";
            if (const auto it = j.find("alignment"); it != j.end() && it->is_object()) {
                const Json* chars = jsonArray(*it, "characters");
                const Json* starts = jsonArray(*it, "character_start_times_seconds");
                const Json* ends = jsonArray(*it, "character_end_times_seconds");
                if (chars && starts && ends && chars->size() == starts->size() && chars->size() == ends->size()) {
                    for (std::size_t i = 0; i < chars->size(); ++i) {
                        const auto t0 = ticksFromSeconds((*starts)[i].get<double>());
                        const auto t1 = ticksFromSeconds((*ends)[i].get<double>());
                        if ((*chars)[i].is_string() && t0 && t1) {
                            result.alignment.push_back(SpokenCharacter{(*chars)[i].get<std::string>(), *t0, *t1});
                        }
                    }
                }
            }
            result.usage.characters = static_cast<std::int64_t>(codePoints(request.text));
            if (!result.alignment.empty()) {
                result.usage.audioMilliseconds = result.alignment.back().t1.value * 1000 / kTicksPerSecond;
            }
            return result;
        });
    }

    // Instant voice cloning: POST /v1/voices/add, multipart name + files -> {"voice_id", "requires_verification"};
    // DELETE /v1/voices/{voice_id} (elevenlabs-python voices/ivc/raw_client.py, checked 2026-10-12).
    std::expected<ClonedVoice, AiError> cloneVoice(std::span<const std::filesystem::path> samples, std::string_view name,
                                                   std::stop_token stop) override {
        if (samples.empty() || samples.size() > 25) {
            return std::unexpected(AiError(AiErrorCode::InvalidRequest, "voice cloning takes 1 to 25 recordings"));
        }
        const ProviderContext c = context();
        HttpRequest http;
        http.method = "POST";
        http.url = joinUrl(m_baseUrl, "/v1/voices/add");
        http.headers = providerHeaders(c);
        http.multipart.push_back({"name", std::string(name.empty() ? std::string_view("Wabcut dub voice") : name), {}, {}, {}});
        http.multipart.push_back({"description", "Made by Wabcut for one dub with the speaker's permission; deleted afterwards.", {}, {}, {}});
        for (const std::filesystem::path& sample : samples) {
            http.multipart.push_back({"files", {}, sample, pathToUtf8(sample.filename()), "audio/wav"});
        }
        http = paidRequest(std::move(http));
        auto response = m_transport.send(http, {}, stop);
        if (!response) {
            return std::unexpected(response.error());
        }
        if (!response->ok()) {
            return std::unexpected(httpError(*response));
        }
        auto parsed = parseJsonBody(response->body, "ElevenLabs");
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        const Json& j = *parsed;
        return guarded("ElevenLabs", [&]() -> std::expected<ClonedVoice, AiError> {
            ClonedVoice voice;
            voice.voiceId = jsonStringOr(j, "voice_id");
            voice.requiresVerification = j.value("requires_verification", false);
            if (voice.voiceId.empty() || !std::all_of(voice.voiceId.begin(), voice.voiceId.end(),
                                                      [](char ch) { return std::isalnum(static_cast<unsigned char>(ch)) != 0; })) {
                return std::unexpected(AiError(AiErrorCode::ProviderError, "ElevenLabs sent no voice id"));
            }
            return voice;
        });
    }

    std::expected<void, AiError> deleteVoice(std::string_view voiceId, std::stop_token stop) override {
        if (voiceId.empty() || !std::all_of(voiceId.begin(), voiceId.end(), [](char ch) { return std::isalnum(static_cast<unsigned char>(ch)) != 0; })) {
            return std::unexpected(AiError(AiErrorCode::InvalidRequest, "not an ElevenLabs voice id"));
        }
        const ProviderContext c = context();
        HttpRequest http;
        http.method = "DELETE";
        http.url = joinUrl(m_baseUrl, "/v1/voices/" + std::string(voiceId));
        http.headers = providerHeaders(c);
        http.timeout = std::chrono::seconds(60);
        auto response = m_transport.send(http, {}, stop);
        if (!response) {
            return std::unexpected(response.error());
        }
        if (!response->ok()) {
            return std::unexpected(httpError(*response));
        }
        return {};
    }
};

// --- OpenAI ---------------------------------------------------------------------------------------------------------

class OpenAiSpeech final : public SpeechBase {
public:
    using SpeechBase::SpeechBase;

    std::expected<SpeechResult, AiError> synthesize(const SpeechRequest& request, std::stop_token stop) override {
        if (auto ok = checkText(request, 4096); !ok) {
            return std::unexpected(ok.error());
        }
        const ProviderContext c = context();
        Json body{{"model", m_spec.id}, {"input", request.text}, {"voice", request.voiceId}, {"response_format", "wav"}};
        if (!request.style.empty()) {
            body["instructions"] = request.style;
        }
        HttpRequest http = paidRequest(jsonRequest(c, endpointUrl(c), body, BodyMode::Buffered));
        auto response = m_transport.send(http, {}, stop);
        if (!response) {
            return std::unexpected(response.error());
        }
        if (!response->ok()) {
            return std::unexpected(httpError(*response));
        }
        SpeechResult result;
        result.audio = bytesOf(response->body);
        const auto duration = wavMilliseconds(result.audio);
        if (!duration) {
            return std::unexpected(AiError(AiErrorCode::ProviderError, "OpenAI sent something that is not a WAV file"));
        }
        result.mediaType = "audio/wav";
        result.usage.characters = static_cast<std::int64_t>(codePoints(request.text));
        result.usage.inputTokens = result.usage.characters / 3 + 1;
        result.usage.audioMilliseconds = *duration;
        return result;
    }
};

// --- Gemini ---------------------------------------------------------------------------------------------------------

class GeminiSpeech final : public SpeechBase {
public:
    using SpeechBase::SpeechBase;

    std::expected<SpeechResult, AiError> synthesize(const SpeechRequest& request, std::stop_token stop) override {
        if (auto ok = checkText(request, 8000); !ok) {
            return std::unexpected(ok.error());
        }
        const ProviderContext c = context();
        Json content{{"type", "text"}, {"text", request.text}};
        if (!request.style.empty()) {
            content["annotations"] = Json::array({Json{{"type", "speech_metadata"}, {"style", request.style}}});
        }
        Json body{{"model", m_spec.id},
                  {"input", Json::array({Json{{"type", "user_input"}, {"content", Json::array({content})}}})},
                  {"response_format", Json{{"type", "audio"}, {"mime_type", "audio/wav"}}},
                  {"generation_config", Json{{"speech_config", Json::array({Json{{"voice", request.voiceId}}})}}}};
        HttpRequest http = paidRequest(jsonRequest(c, joinUrl(m_baseUrl, "/v1beta/interactions"), body, BodyMode::Buffered));
        auto response = m_transport.send(http, {}, stop);
        if (!response) {
            return std::unexpected(response.error());
        }
        if (!response->ok()) {
            return std::unexpected(httpError(*response));
        }
        auto parsed = parseJsonBody(response->body, "Gemini");
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        const Json& j = *parsed;
        return guarded("Gemini", [&]() -> std::expected<SpeechResult, AiError> {
            std::string data;
            if (const auto it = j.find("output_audio"); it != j.end() && it->is_object()) {
                data = jsonStringOr(*it, "data");
            }
            if (data.empty()) {
                if (const Json* steps = jsonArray(j, "steps")) {
                    for (const Json& step : *steps) {
                        if (jsonStringOr(step, "type") != "model_output") {
                            continue;
                        }
                        if (const Json* parts = jsonArray(step, "content")) {
                            for (const Json& part : *parts) {
                                if (jsonStringOr(part, "type") == "audio") {
                                    data = jsonStringOr(part, "data");
                                }
                            }
                        }
                    }
                }
            }
            auto audio = base64Decode(data);
            if (!audio || audio->empty()) {
                return std::unexpected(AiError(AiErrorCode::ProviderError, "Gemini sent no audio"));
            }
            const auto duration = wavMilliseconds(*audio);
            if (!duration) {
                return std::unexpected(AiError(AiErrorCode::ProviderError, "Gemini sent audio that is not a WAV file"));
            }
            SpeechResult result;
            result.audio = std::move(*audio);
            result.mediaType = "audio/wav";
            result.usage.characters = static_cast<std::int64_t>(codePoints(request.text));
            result.usage.inputTokens = result.usage.characters / 3 + 1;
            result.usage.audioMilliseconds = *duration;
            return result;
        });
    }
};

} // namespace

std::unique_ptr<ISpeechSynth> makeElevenLabsSpeech(ProviderContext context) { return std::make_unique<ElevenLabsSpeech>(std::move(context)); }
std::unique_ptr<ISpeechSynth> makeOpenAiSpeech(ProviderContext context) { return std::make_unique<OpenAiSpeech>(std::move(context)); }
std::unique_ptr<ISpeechSynth> makeGeminiSpeech(ProviderContext context) { return std::make_unique<GeminiSpeech>(std::move(context)); }

} // namespace wabcut::ai::net
