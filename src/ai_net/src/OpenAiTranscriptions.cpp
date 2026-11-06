// openai-transcriptions (r07): POST {base}{endpoint = /audio/transcriptions}, multipart (file, model, response_format,
// timestamp_granularities[], language), the bearer key. The OpenAI-compatible speech-to-text call of OpenAI (whisper-1,
// gpt-4o-transcribe-diarize), Groq, Mistral (Voxtral), Together, Fireworks, DeepInfra and custom providers.
//   - Timings: response_format=verbose_json with timestamp_granularities[]=segment, plus =word for models whose registry
//     entry has wordTimestamps (whisper-1, Groq, Together, DeepInfra) -> {"language", "duration", "text", "words":
//     [{"word" | "text", "start", "end", "speaker_id"?}], "segments": [{"start", "end", "text", "speaker_id"?}]} (seconds).
//     Without words, each segment's words get times spread over the segment by their length.
//   - Speakers: models whose id names "diarize" (gpt-4o-transcribe-diarize) get response_format=diarized_json and
//     chunking_strategy=auto -> {"segments": [{"start", "end", "text", "speaker"}]}; other models with diarization in
//     the registry (Mistral, Together) get diarize=true.
//   - Mistral takes no response_format: its JSON answer carries the segments when timestamp_granularities is set, and
//     rejects word granularity.
// Checked on 2026-11-03 against developers.openai.com/api/reference/resources/audio/subresources/transcriptions/methods/create,
// console.groq.com/docs/speech-to-text, docs.mistral.ai/studio/audio/speech-to-text/transcribe-files,
// docs.together.ai/reference/audio-transcriptions and docs.deepinfra.com/api-reference/audio/openai-audio-transcriptions.

#include "AdapterUtil.h"

#include <wabcut/ai/JsonAccess.h>
#include <wabcut/ai_net/Adapters.h>
#include <wabcut/model/MediaLocator.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>

namespace wabcut::ai::net {

namespace {

/// Whisper's verbose_json names the language ("english"); everything else sends a code.
std::string languageCode(std::string language) {
    for (char& c : language) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    static const std::map<std::string, std::string, std::less<>> kNames{
        {"english", "en"},   {"thai", "th"},      {"japanese", "ja"},  {"korean", "ko"},     {"chinese", "zh"},   {"spanish", "es"},
        {"french", "fr"},    {"german", "de"},    {"italian", "it"},   {"portuguese", "pt"}, {"russian", "ru"},   {"dutch", "nl"},
        {"polish", "pl"},    {"turkish", "tr"},   {"arabic", "ar"},    {"hindi", "hi"},      {"vietnamese", "vi"}, {"indonesian", "id"},
        {"swedish", "sv"},   {"ukrainian", "uk"}, {"czech", "cs"},     {"greek", "el"},      {"hebrew", "he"},    {"danish", "da"},
        {"finnish", "fi"},   {"norwegian", "no"}, {"hungarian", "hu"}, {"romanian", "ro"},   {"malay", "ms"},     {"tagalog", "tl"},
    };
    if (auto it = kNames.find(language); it != kNames.end()) {
        return it->second;
    }
    return normalizeLanguage(language);
}

/// "speaker" (OpenAI diarized_json) or "speaker_id" (Mistral, Together); empty when unknown.
std::string speakerOf(const Json& j) {
    std::string speaker = jsonIdOr(j, "speaker");
    return speaker.empty() ? jsonIdOr(j, "speaker_id") : speaker;
}

Ticks seconds(const Json& j, const char* key) {
    const auto value = jsonNumber(j, key);
    return value ? ticksFromSeconds(*value).value_or(Ticks{}) : Ticks{};
}

/// Splits a segment's text into words and spreads them over [t0, t1] by their length.
void spreadWords(TranscriptBuilder& builder, std::string_view text, Ticks t0, Ticks t1, std::string_view speaker) {
    std::vector<std::string> words;
    std::string word;
    for (const char c : text) {
        if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
            if (!word.empty()) {
                words.push_back(std::move(word));
                word.clear();
            }
        } else {
            word += c;
        }
    }
    if (!word.empty()) {
        words.push_back(std::move(word));
    }
    if (words.empty()) {
        return;
    }
    std::int64_t total = 0;
    for (const auto& w : words) {
        total += std::max<std::int64_t>(1, codePointCount(w));
    }
    const std::int64_t span = std::max<std::int64_t>(0, t1.value - t0.value);
    std::int64_t done = 0;
    for (auto& w : words) {
        const std::int64_t length = std::max<std::int64_t>(1, codePointCount(w));
        const Ticks a{t0.value + span * done / total};
        done += length;
        const Ticks b{t0.value + span * done / total};
        builder.addWord(std::move(w), a, b, 0.0f, speaker);
    }
}

class OpenAiTranscriber final : public ITranscriber {
public:
    explicit OpenAiTranscriber(ProviderContext context)
        : m_provider(context.provider), m_spec(context.model), m_baseUrl(std::move(context.baseUrl)), m_key(std::move(context.key)),
          m_transport(context.transport) {}

    const ModelSpec& spec() const override { return m_spec; }

    std::expected<TranscribeResult, AiError> transcribe(const TranscribeRequest& request, const ProgressFn& progress,
                                                        std::stop_token stop) override {
        std::error_code ec;
        const auto size = std::filesystem::file_size(request.audioFile, ec);
        if (ec) {
            return std::unexpected(AiError(AiErrorCode::InvalidRequest, "cannot read the audio file"));
        }
        if (m_spec.maxUploadBytes > 0 && static_cast<std::int64_t>(size) > m_spec.maxUploadBytes) {
            return std::unexpected(AiError(AiErrorCode::PayloadTooLarge, "the audio file is larger than " + m_spec.name + " accepts"));
        }
        if (m_spec.maxAudioSeconds > 0 && request.audioMilliseconds > m_spec.maxAudioSeconds * 1000) {
            return std::unexpected(AiError(AiErrorCode::PayloadTooLarge, "the audio is longer than " + m_spec.name + " accepts"));
        }
        const bool diarized = m_spec.id.find("diarize") != std::string::npos;
        const ProviderContext context{m_provider, m_spec, m_baseUrl, m_key.clone(), m_transport};
        HttpRequest http;
        http.method = "POST";
        http.url = endpointUrl(context);
        http.headers = providerHeaders(context);
        http.multipart.push_back({"model", m_spec.id, {}, {}, {}});
        http.multipart.push_back({"file", {}, request.audioFile, pathToUtf8(request.audioFile.filename()), request.mediaType});
        const bool mistral = m_provider.id == "mistral";
        if (diarized) {
            http.multipart.push_back({"response_format", "diarized_json", {}, {}, {}});
            http.multipart.push_back({"chunking_strategy", "auto", {}, {}, {}}); // required above 30 s
        } else {
            if (!mistral) {
                http.multipart.push_back({"response_format", "verbose_json", {}, {}, {}});
            }
            if (m_spec.caps.wordTimestamps) {
                http.multipart.push_back({"timestamp_granularities[]", "word", {}, {}, {}});
            }
            http.multipart.push_back({"timestamp_granularities[]", "segment", {}, {}, {}});
            if (request.diarize && m_spec.caps.diarization) {
                http.multipart.push_back({"diarize", "true", {}, {}, {}});
            }
        }
        if (!request.language.empty()) {
            http.multipart.push_back({"language", normalizeLanguage(request.language), {}, {}, {}});
        }
        http.retrySafe = false; // billed per minute: an ambiguous failure is not sent twice
        http.timeout = std::chrono::hours(1);
        http.idleTimeout = std::chrono::minutes(11);
        HttpStreamHandlers handlers;
        if (progress) {
            handlers.onUploadProgress = [&](std::uint64_t sent, std::uint64_t total) {
                progress(total ? 0.9f * static_cast<float>(sent) / static_cast<float>(total) : 0.0f);
            };
        }
        auto response = m_transport.send(http, handlers, stop);
        if (!response) {
            return std::unexpected(response.error());
        }
        if (!response->ok()) {
            return std::unexpected(httpError(*response));
        }
        auto body = parseJsonBody(response->body, m_provider.name);
        if (!body) {
            return std::unexpected(body.error());
        }
        const Json& j = *body;
        try {
            TranscriptBuilder builder(request);
            builder.setLanguage(request.language.empty() ? languageCode(jsonStringOr(j, "language")) : normalizeLanguage(request.language));
            Ticks end{};
            const Json* words = jsonArray(j, "words");
            const Json* segments = jsonArray(j, "segments");
            if (words && !words->empty() && !diarized) {
                for (const Json& w : *words) {
                    std::string text = jsonStringOr(w, "word");
                    if (text.empty()) {
                        text = jsonStringOr(w, "text");
                    }
                    const Ticks t1 = seconds(w, "end");
                    end = std::max(end, t1);
                    builder.addWord(std::move(text), seconds(w, "start"), t1, 0.0f, speakerOf(w));
                }
                if (segments) {
                    for (const Json& s : *segments) {
                        builder.addUtterance(seconds(s, "start"), seconds(s, "end"));
                    }
                }
            } else if (segments && !segments->empty()) {
                for (const Json& s : *segments) {
                    const Ticks t0 = seconds(s, "start");
                    const Ticks t1 = std::max(t0, seconds(s, "end"));
                    end = std::max(end, t1);
                    spreadWords(builder, jsonStringOr(s, "text"), t0, t1, speakerOf(s));
                    builder.addUtterance(t0, t1);
                }
            } else {
                // Text only (a model or server without timings): one segment over the whole audio.
                const auto duration = jsonNumber(j, "duration");
                const Ticks t1 = duration ? ticksFromSeconds(*duration).value_or(Ticks{})
                                          : ticksFromMilliseconds(request.audioMilliseconds).value_or(Ticks{});
                if (t1.value <= 0) {
                    return std::unexpected(AiError(AiErrorCode::ProviderError, m_provider.name + " sent text without any timing"));
                }
                end = t1;
                spreadWords(builder, jsonStringOr(j, "text"), Ticks{}, t1, {});
            }
            auto transcript = builder.build(cloudProvenance(context, request, m_spec.id, {}));
            if (!transcript) {
                return std::unexpected(transcript.error());
            }
            TranscribeResult result;
            result.transcript = std::move(*transcript);
            // The length the provider measured or billed: "duration" (OpenAI, Groq, Together), usage.seconds (OpenAI),
            // usage.prompt_audio_seconds (Mistral); else the file's own length; else up to the last word.
            const Ticks fileLength = ticksFromMilliseconds(request.audioMilliseconds).value_or(Ticks{});
            const auto duration = jsonNumber(j, "duration");
            result.duration = duration ? ticksFromSeconds(*duration).value_or(end) : fileLength.value > 0 ? fileLength : end;
            for (const auto billed : {jsonAt(j, {"usage", "seconds"}), jsonAt(j, {"usage", "prompt_audio_seconds"})}) {
                if (billed && asNumber(billed)) {
                    result.duration = ticksFromSeconds(*asNumber(billed)).value_or(result.duration);
                    break;
                }
            }
            result.usage.audioMilliseconds = result.duration.value / (kTicksPerSecond / 1000);
            if (result.usage.audioMilliseconds <= 0) {
                result.usage.audioMilliseconds = request.audioMilliseconds;
            }
            if (auto id = findHeader(response->headers, "x-request-id")) {
                result.requestId = std::string(*id);
            }
            if (progress) {
                progress(1.0f);
            }
            return result;
        } catch (const std::exception& e) {
            return std::unexpected(AiError(AiErrorCode::ProviderError, "unreadable " + m_provider.name + " response: " + e.what()));
        }
    }

private:
    ProviderSpec m_provider;
    ModelSpec m_spec;
    std::string m_baseUrl;
    Secret m_key;
    IHttpTransport& m_transport;
};

} // namespace

std::unique_ptr<ITranscriber> makeOpenAiTranscriber(ProviderContext context) { return std::make_unique<OpenAiTranscriber>(std::move(context)); }

} // namespace wabcut::ai::net
