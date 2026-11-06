// Text-to-speech adapters added in round 07 (docs/ai.md "Providers"), checked against the vendors' docs on 2026-11-03:
//   deepgram-speak  POST {base}/v1/speak?model=aura-2-<voice>-<lang>&encoding=linear16&container=wav&sample_rate=24000
//                   &mip_opt_out=true, Authorization: Token, {"text"} -> the WAV file. 2,000 characters a request;
//                   $0.030 per 1k characters (Aura-2). developers.deepgram.com/reference/text-to-speech/speak-request
//   cartesia-tts    POST {base}/tts/bytes, Authorization: Bearer, Cartesia-Version (provider header), {"model_id",
//                   "transcript", "voice": "<voice id>", "output_format": {"container": "wav", "encoding": "pcm_s16le",
//                   "sample_rate": 44100}, "language"} -> the WAV file. docs.cartesia.ai/api-reference/tts/bytes
//   fish-tts        POST {base}/v1/tts, Authorization: Bearer, "model: <model id>" header, {"text", "reference_id",
//                   "format": "wav"} -> the WAV file. Billed per UTF-8 byte.
//                   docs.fish.audio/api-reference/endpoint/openapi-v1/text-to-speech
// None of them is retried: a network ambiguity must not bill a line twice. None of them clones voices here.

#include "AdapterUtil.h"

#include <wabcut/ai/JsonAccess.h>
#include <wabcut/ai_net/Adapters.h>

#include <algorithm>
#include <cctype>

namespace wabcut::ai::net {

namespace {

bool voiceChars(std::string_view voice, std::string_view extra) {
    return !voice.empty() && voice.size() <= 128 && std::all_of(voice.begin(), voice.end(), [&](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || extra.find(c) != std::string_view::npos;
    });
}

class VoiceBase : public ISpeechSynth {
public:
    explicit VoiceBase(ProviderContext context)
        : m_provider(context.provider), m_spec(context.model), m_baseUrl(std::move(context.baseUrl)), m_key(std::move(context.key)),
          m_transport(context.transport) {}

    const ModelSpec& spec() const override { return m_spec; }

protected:
    ProviderContext context() const { return ProviderContext{m_provider, m_spec, m_baseUrl, m_key.clone(), m_transport}; }

    std::expected<void, AiError> checkText(const SpeechRequest& request, std::int64_t limit) const {
        if (request.text.empty()) {
            return std::unexpected(AiError(AiErrorCode::InvalidRequest, "nothing to say"));
        }
        if (codePointCount(request.text) > limit) {
            return std::unexpected(AiError(AiErrorCode::PayloadTooLarge, "the line is longer than the voice model takes (" +
                                                                             std::to_string(limit) + " characters)"));
        }
        if (request.voiceId.empty()) {
            return std::unexpected(AiError(AiErrorCode::InvalidRequest, "no voice chosen"));
        }
        return {};
    }

    /// Sends the request and turns the audio answer into a SpeechResult (WAV or MP3, measured).
    std::expected<SpeechResult, AiError> audioAnswer(HttpRequest http, const SpeechRequest& request, std::string_view vendor,
                                                     std::stop_token stop) {
        auto response = m_transport.send(paidSubmit(std::move(http), std::chrono::seconds(120)), {}, stop);
        if (!response) {
            return std::unexpected(response.error());
        }
        if (!response->ok()) {
            return std::unexpected(httpError(*response));
        }
        SpeechResult result;
        result.audio.assign(response->body.begin(), response->body.end());
        result.mediaType = sniffAudioType(result.audio);
        const auto duration = audioMilliseconds(result.audio);
        if (result.mediaType.empty() || !duration) {
            return std::unexpected(AiError(AiErrorCode::ProviderError, std::string(vendor) + " sent something that is not a WAV or MP3 file"));
        }
        result.usage.characters = codePointCount(request.text);
        result.usage.audioMilliseconds = *duration;
        return result;
    }

    ProviderSpec m_provider;
    ModelSpec m_spec;
    std::string m_baseUrl;
    Secret m_key;
    IHttpTransport& m_transport;
};

// --- Deepgram Aura --------------------------------------------------------------------------------------------------

class DeepgramSpeech final : public VoiceBase {
public:
    using VoiceBase::VoiceBase;

    std::expected<SpeechResult, AiError> synthesize(const SpeechRequest& request, std::stop_token stop) override {
        if (auto ok = checkText(request, 2000); !ok) {
            return std::unexpected(ok.error());
        }
        // A voice is a model on Deepgram: "thalia-en" on aura-2 is model=aura-2-thalia-en; a full id is taken as it is.
        if (!voiceChars(request.voiceId, "-")) {
            return std::unexpected(AiError(AiErrorCode::InvalidRequest, "not a Deepgram voice"));
        }
        const std::string voice = request.voiceId.starts_with("aura-") ? request.voiceId : m_spec.id + "-" + request.voiceId;
        const ProviderContext c = context();
        const std::string url = joinUrl(m_baseUrl, m_spec.endpoint.empty() ? std::string_view("/v1/speak") : std::string_view(m_spec.endpoint)) +
                                "?model=" + voice + "&encoding=linear16&container=wav&sample_rate=24000&mip_opt_out=true";
        return audioAnswer(jsonRequest(c, url, Json{{"text", request.text}}, BodyMode::Buffered), request, "Deepgram", stop);
    }
};

// --- Cartesia -------------------------------------------------------------------------------------------------------

class CartesiaSpeech final : public VoiceBase {
public:
    using VoiceBase::VoiceBase;

    std::expected<SpeechResult, AiError> synthesize(const SpeechRequest& request, std::stop_token stop) override {
        if (auto ok = checkText(request, 10000); !ok) {
            return std::unexpected(ok.error());
        }
        if (!voiceChars(request.voiceId, "-_")) {
            return std::unexpected(AiError(AiErrorCode::InvalidRequest, "not a Cartesia voice id"));
        }
        Json body{{"model_id", m_spec.id},
                  {"transcript", request.text},
                  {"voice", request.voiceId}, // 2026-08-14: a voice id string (the docs' own form) or {"id"}
                  {"output_format", Json{{"container", "wav"}, {"encoding", "pcm_s16le"}, {"sample_rate", 44100}}}};
        if (!request.language.empty()) {
            body["language"] = normalizeLanguage(request.language);
        }
        const ProviderContext c = context();
        return audioAnswer(jsonRequest(c, endpointUrl(c), body, BodyMode::Buffered), request, "Cartesia", stop);
    }
};

// --- Fish Audio -----------------------------------------------------------------------------------------------------

class FishSpeech final : public VoiceBase {
public:
    using VoiceBase::VoiceBase;

    std::expected<SpeechResult, AiError> synthesize(const SpeechRequest& request, std::stop_token stop) override {
        if (auto ok = checkText(request, 10000); !ok) {
            return std::unexpected(ok.error());
        }
        if (!voiceChars(request.voiceId, "-_")) {
            return std::unexpected(AiError(AiErrorCode::InvalidRequest, "not a Fish Audio voice id"));
        }
        Json body{{"text", request.text}, {"reference_id", request.voiceId}, {"format", "wav"}, {"sample_rate", 44100}};
        const ProviderContext c = context();
        HttpRequest http = jsonRequest(c, endpointUrl(c), body, BodyMode::Buffered);
        http.headers.push_back({"model", m_spec.id});
        auto result = audioAnswer(std::move(http), request, "Fish Audio", stop);
        if (result) {
            result->usage.characters = static_cast<std::int64_t>(request.text.size()); // billed per UTF-8 byte
        }
        return result;
    }
};

} // namespace

std::unique_ptr<ISpeechSynth> makeDeepgramSpeech(ProviderContext context) { return std::make_unique<DeepgramSpeech>(std::move(context)); }
std::unique_ptr<ISpeechSynth> makeCartesiaSpeech(ProviderContext context) { return std::make_unique<CartesiaSpeech>(std::move(context)); }
std::unique_ptr<ISpeechSynth> makeFishSpeech(ProviderContext context) { return std::make_unique<FishSpeech>(std::move(context)); }

} // namespace wabcut::ai::net
