// Cloud transcription adapters, returning the shared Transcript (Transcript.h) in source time.
//
// Deepgram  developers.deepgram.com/reference/speech-to-text/listen-pre-recorded: raw audio body, Authorization:
//           Token; diarize_model=latest (diarize is deprecated, both together are rejected); mip_opt_out=true keeps
//           requests out of Deepgram's model-improvement programme; words in seconds.
// ElevenLabs elevenlabs.io/docs/api-reference/speech-to-text/convert: multipart, xi-api-key, model_id scribe_v2,
//           timestamps_granularity=word; words in seconds with type word|spacing|audio_event and speaker_id.
// AssemblyAI assemblyai.com/docs/api-reference: /v2/upload -> /v2/transcript (speech_models, speaker_labels) -> poll;
//           raw-key Authorization; words in milliseconds, speakers "A", "B", ...
// All verified 2026-10-11.

#include "AdapterUtil.h"

#include <wabcut/ai/JsonAccess.h>

#include <wabcut/ai_net/Adapters.h>
#include <wabcut/model/MediaLocator.h>

#include <cmath>
#include <filesystem>
#include <system_error>

namespace wabcut::ai::net {

namespace {

std::expected<std::int64_t, AiError> checkAudioFile(const TranscribeRequest& request, const ModelSpec& spec) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(request.audioFile, ec);
    if (ec) {
        return std::unexpected(AiError(AiErrorCode::InvalidRequest, "cannot read the audio file"));
    }
    if (spec.maxUploadBytes > 0 && static_cast<std::int64_t>(size) > spec.maxUploadBytes) {
        return std::unexpected(AiError(AiErrorCode::PayloadTooLarge, "the audio file is larger than " + spec.name + " accepts"));
    }
    if (spec.maxAudioSeconds > 0 && request.audioMilliseconds > spec.maxAudioSeconds * 1000) {
        return std::unexpected(AiError(AiErrorCode::PayloadTooLarge, "the audio is longer than " + spec.name + " accepts"));
    }
    return static_cast<std::int64_t>(size);
}

std::string boolString(bool b) { return b ? "true" : "false"; }

Ticks secondsOr(const Json& j, const char* key, Ticks fallback) {
    const auto seconds = jsonNumber(j, key);
    return seconds ? ticksFromSeconds(*seconds).value_or(fallback) : fallback;
}

/// A speaker label sent as a string or a number; anything else (null, objects, arrays) is "unknown".
std::string speakerLabel(const Json& word, const char* key) { return jsonIdOr(word, key); }

/// A confidence in [0, 1]; out of range or not a number is 0.
float confidenceOf(const Json& word, const char* key) {
    const double c = jsonNumberOr(word, key, 0.0);
    return c >= 0.0 && c <= 1.0 ? static_cast<float>(c) : 0.0f;
}

/// A provider resource id that goes into a URL path: letters, digits, '-' and '_' only.
bool isPathId(std::string_view id) {
    return !id.empty() && id.size() <= 128 && std::all_of(id.begin(), id.end(), [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
    });
}

/// Turns an exception from parsing a provider response into ProviderError (defence in depth: the accessors above
/// do not throw, but a provider response must never take the app down).
template <class Parse>
auto guardedParse(std::string_view provider, Parse&& parse) -> decltype(parse()) {
    try {
        return parse();
    } catch (const std::exception& e) {
        return std::unexpected(AiError(AiErrorCode::ProviderError, "unreadable " + std::string(provider) + " response: " + e.what()));
    }
}

std::int64_t millisecondsOf(Ticks t) { return t.value / (kTicksPerSecond / 1000); }

/// Common long-request settings: the provider processes the whole file before answering.
void longRequest(HttpRequest& request) {
    request.timeout = std::chrono::hours(2);
    request.idleTimeout = std::chrono::minutes(11); // Deepgram answers 504 after 10 minutes of processing
}

// --- Deepgram ---------------------------------------------------------------------------------------------------

class DeepgramTranscriber final : public ITranscriber {
public:
    explicit DeepgramTranscriber(ProviderContext context)
        : m_provider(context.provider), m_spec(context.model), m_baseUrl(std::move(context.baseUrl)),
          m_key(std::move(context.key)), m_transport(context.transport) {}

    const ModelSpec& spec() const override { return m_spec; }

    std::expected<TranscribeResult, AiError> transcribe(const TranscribeRequest& request, const ProgressFn& progress,
                                                        std::stop_token stop) override {
        if (auto size = checkAudioFile(request, m_spec); !size) {
            return std::unexpected(size.error());
        }
        const ProviderContext context{m_provider, m_spec, m_baseUrl, m_key.clone(), m_transport};
        std::string url = endpointUrl(context) + "?model=" + m_spec.id + "&smart_format=true&utterances=true&mip_opt_out=true";
        if (request.diarize) {
            url += "&diarize_model=latest";
        }
        url += request.language.empty() ? "&detect_language=true" : "&language=" + request.language;

        HttpRequest http;
        http.method = "POST";
        http.url = std::move(url);
        http.headers = providerHeaders(context);
        http.headers.push_back({"content-type", request.mediaType});
        http.bodyFile = request.audioFile;
        longRequest(http);
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
        auto body = parseJsonBody(response->body, "Deepgram");
        if (!body) {
            return std::unexpected(body.error());
        }
        const Json& j = *body;
        return guardedParse("Deepgram", [&]() -> std::expected<TranscribeResult, AiError> {
            const Json none = Json::object();
            const Json* channels = jsonAt(j, {"results", "channels"});
            const Json& channel =
                channels && channels->is_array() && !channels->empty() && (*channels)[0].is_object() ? (*channels)[0] : none;
            const Json* alternatives = jsonArray(channel, "alternatives");
            const Json& alternative =
                alternatives && !alternatives->empty() && (*alternatives)[0].is_object() ? (*alternatives)[0] : none;
            TranscriptBuilder builder(request);
            builder.setLanguage(jsonStringOr(channel, "detected_language"));
            if (const Json* words = jsonArray(alternative, "words")) {
                for (const auto& w : *words) {
                    std::string text = jsonStringOr(w, "punctuated_word");
                    if (text.empty()) {
                        text = jsonStringOr(w, "word");
                    }
                    builder.addWord(std::move(text), secondsOr(w, "start", Ticks{}), secondsOr(w, "end", Ticks{}),
                                    confidenceOf(w, "confidence"), speakerLabel(w, "speaker"));
                }
            }
            if (const Json* utterances = jsonAt(j, {"results", "utterances"}); utterances && utterances->is_array()) {
                for (const auto& u : *utterances) {
                    builder.addUtterance(secondsOr(u, "start", Ticks{}), secondsOr(u, "end", Ticks{}));
                }
            }
            std::string modelVersion;
            if (const Json* models = jsonAt(j, {"metadata", "models"}); models && models->is_array() && !models->empty()) {
                modelVersion = asString(&(*models)[0]).value_or("");
            }
            auto transcript = builder.build(cloudProvenance(context, request, m_spec.id, modelVersion));
            if (!transcript) {
                return std::unexpected(transcript.error());
            }
            TranscribeResult result;
            result.transcript = std::move(*transcript);
            const Json* metadata = jsonObject(j, "metadata");
            result.duration = secondsOr(metadata ? *metadata : none, "duration", Ticks{});
            result.usage.audioMilliseconds = millisecondsOf(result.duration);
            result.requestId = stringAt(j, {"metadata", "request_id"});
            if (progress) {
                progress(1.0f);
            }
            return result;
        });
    }

private:
    ProviderSpec m_provider;
    ModelSpec m_spec;
    std::string m_baseUrl;
    Secret m_key;
    IHttpTransport& m_transport;
};

// --- ElevenLabs Scribe ------------------------------------------------------------------------------------------

class ElevenLabsTranscriber final : public ITranscriber {
public:
    explicit ElevenLabsTranscriber(ProviderContext context)
        : m_provider(context.provider), m_spec(context.model), m_baseUrl(std::move(context.baseUrl)),
          m_key(std::move(context.key)), m_transport(context.transport) {}

    const ModelSpec& spec() const override { return m_spec; }

    std::expected<TranscribeResult, AiError> transcribe(const TranscribeRequest& request, const ProgressFn& progress,
                                                        std::stop_token stop) override {
        if (auto size = checkAudioFile(request, m_spec); !size) {
            return std::unexpected(size.error());
        }
        const ProviderContext context{m_provider, m_spec, m_baseUrl, m_key.clone(), m_transport};
        HttpRequest http;
        http.method = "POST";
        http.url = endpointUrl(context);
        http.headers = providerHeaders(context);
        http.multipart.push_back({"model_id", m_spec.id, {}, {}, {}});
        http.multipart.push_back({"file", {}, request.audioFile, pathToUtf8(request.audioFile.filename()), request.mediaType});
        if (!request.language.empty()) {
            http.multipart.push_back({"language_code", request.language, {}, {}, {}});
        }
        http.multipart.push_back({"diarize", boolString(request.diarize), {}, {}, {}});
        if (request.diarize && request.speakerCount) {
            http.multipart.push_back({"num_speakers", std::to_string(*request.speakerCount), {}, {}, {}});
        }
        http.multipart.push_back({"timestamps_granularity", "word", {}, {}, {}});
        http.multipart.push_back({"tag_audio_events", "false", {}, {}, {}});
        longRequest(http);
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
        auto body = parseJsonBody(response->body, "ElevenLabs");
        if (!body) {
            return std::unexpected(body.error());
        }
        const Json& j = *body;
        return guardedParse("ElevenLabs", [&]() -> std::expected<TranscribeResult, AiError> {
            TranscriptBuilder builder(request);
            builder.setLanguage(jsonStringOr(j, "language_code"));
            if (const Json* words = jsonArray(j, "words")) {
                for (const auto& w : *words) {
                    if (jsonStringOr(w, "type", "word") != "word") {
                        continue; // spacing and audio events
                    }
                    const double logprob = jsonNumberOr(w, "logprob", 0.0);
                    builder.addWord(jsonStringOr(w, "text"), secondsOr(w, "start", Ticks{}), secondsOr(w, "end", Ticks{}),
                                    static_cast<float>(std::exp(std::min(0.0, logprob))), speakerLabel(w, "speaker_id"));
                }
            }
            auto transcript = builder.build(cloudProvenance(context, request, m_spec.id, {}));
            if (!transcript) {
                return std::unexpected(transcript.error());
            }
            TranscribeResult result;
            result.transcript = std::move(*transcript);
            result.duration = secondsOr(j, "audio_duration_secs", Ticks{});
            result.usage.audioMilliseconds = millisecondsOf(result.duration);
            result.requestId = jsonStringOr(j, "transcription_id");
            if (progress) {
                progress(1.0f);
            }
            return result;
        });
    }

private:
    ProviderSpec m_provider;
    ModelSpec m_spec;
    std::string m_baseUrl;
    Secret m_key;
    IHttpTransport& m_transport;
};

// --- AssemblyAI -------------------------------------------------------------------------------------------------

class AssemblyAiTranscriber final : public ITranscriber {
public:
    AssemblyAiTranscriber(ProviderContext context, AdapterOptions options)
        : m_provider(context.provider), m_spec(context.model), m_baseUrl(std::move(context.baseUrl)),
          m_key(std::move(context.key)), m_transport(context.transport), m_options(std::move(options)) {}

    const ModelSpec& spec() const override { return m_spec; }

    std::expected<TranscribeResult, AiError> transcribe(const TranscribeRequest& request, const ProgressFn& progress,
                                                        std::stop_token stop) override {
        if (auto size = checkAudioFile(request, m_spec); !size) {
            return std::unexpected(size.error());
        }
        const ProviderContext context{m_provider, m_spec, m_baseUrl, m_key.clone(), m_transport};
        auto report = [&](float value) {
            if (progress) {
                progress(value);
            }
        };

        // 1. Upload the audio.
        HttpRequest upload;
        upload.method = "POST";
        upload.url = joinUrl(m_baseUrl, "/v2/upload");
        upload.headers = providerHeaders(context);
        upload.headers.push_back({"content-type", "application/octet-stream"});
        upload.bodyFile = request.audioFile;
        upload.timeout = std::chrono::hours(2);
        HttpStreamHandlers uploadHandlers;
        uploadHandlers.onUploadProgress = [&](std::uint64_t sent, std::uint64_t total) {
            report(total ? 0.4f * static_cast<float>(sent) / static_cast<float>(total) : 0.0f);
        };
        auto uploaded = m_transport.send(upload, uploadHandlers, stop);
        if (!uploaded) {
            return std::unexpected(uploaded.error());
        }
        if (!uploaded->ok()) {
            return std::unexpected(httpError(*uploaded));
        }
        auto uploadBody = parseJsonBody(uploaded->body, "AssemblyAI upload");
        if (!uploadBody) {
            return std::unexpected(uploadBody.error());
        }
        const std::string audioUrl = jsonStringOr(*uploadBody, "upload_url");
        if (audioUrl.empty()) {
            return std::unexpected(AiError(AiErrorCode::ProviderError, "AssemblyAI returned no upload_url"));
        }

        // 2. Submit.
        Json submit = {{"audio_url", audioUrl}, {"speaker_labels", request.diarize}, {"punctuate", true}, {"format_text", true}};
        Json models = Json::array({m_spec.id});
        if (m_spec.id != "universal-2") {
            models.push_back("universal-2"); // fallback for languages the first model does not cover
        }
        submit["speech_models"] = std::move(models);
        if (request.language.empty()) {
            submit["language_detection"] = true;
        } else {
            submit["language_code"] = request.language;
        }
        if (request.diarize && request.speakerCount) {
            submit["speakers_expected"] = *request.speakerCount;
        }
        auto submitted = m_transport.send(jsonRequest(context, endpointUrl(context), submit, BodyMode::Buffered), {}, stop);
        if (!submitted) {
            return std::unexpected(submitted.error());
        }
        if (!submitted->ok()) {
            return std::unexpected(httpError(*submitted));
        }
        auto job = parseJsonBody(submitted->body, "AssemblyAI submit");
        if (!job) {
            return std::unexpected(job.error());
        }
        const std::string id = jsonStringOr(*job, "id");
        if (!isPathId(id)) { // it becomes part of the poll URL
            return std::unexpected(AiError(AiErrorCode::ProviderError, "AssemblyAI returned no valid transcript id"));
        }
        report(0.45f);

        // 3. Poll until completed or error.
        Json transcriptJson = *job;
        const auto deadline = std::chrono::steady_clock::now() + m_options.maxPollTime;
        while (true) {
            const std::string status = jsonStringOr(transcriptJson, "status", "queued");
            if (status == "completed") {
                break;
            }
            if (status == "error") {
                return std::unexpected(AiError(AiErrorCode::ProviderError, "AssemblyAI: " + jsonStringOr(transcriptJson, "error")));
            }
            report(status == "processing" ? 0.7f : 0.5f);
            if (std::chrono::steady_clock::now() > deadline) {
                return std::unexpected(AiError(AiErrorCode::Network, "AssemblyAI did not finish before the deadline"));
            }
            if (!m_options.sleeper(m_options.pollInterval, stop)) {
                return std::unexpected(AiError(AiErrorCode::Cancelled, "cancelled while AssemblyAI was transcribing"));
            }
            HttpRequest poll;
            poll.method = "GET";
            poll.url = joinUrl(m_baseUrl, "/v2/transcript/" + id);
            poll.headers = providerHeaders(context);
            auto polled = m_transport.send(poll, {}, stop);
            if (!polled) {
                return std::unexpected(polled.error());
            }
            if (!polled->ok()) {
                return std::unexpected(httpError(*polled));
            }
            auto parsed = parseJsonBody(polled->body, "AssemblyAI transcript");
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            transcriptJson = std::move(*parsed);
        }

        // 4. Words in milliseconds.
        const Json& j = transcriptJson;
        return guardedParse("AssemblyAI", [&]() -> std::expected<TranscribeResult, AiError> {
            TranscriptBuilder builder(request);
            builder.setLanguage(jsonStringOr(j, "language_code"));
            auto ms = [](const Json& w, const char* key) {
                const auto value = jsonInt(w, key);
                return value ? ticksFromMilliseconds(*value).value_or(Ticks{}) : Ticks{};
            };
            if (const Json* words = jsonArray(j, "words")) {
                for (const auto& w : *words) {
                    builder.addWord(jsonStringOr(w, "text"), ms(w, "start"), ms(w, "end"), confidenceOf(w, "confidence"),
                                    speakerLabel(w, "speaker"));
                }
            }
            if (const Json* utterances = jsonArray(j, "utterances")) {
                for (const auto& u : *utterances) {
                    builder.addUtterance(ms(u, "start"), ms(u, "end"));
                }
            }
            auto transcript = builder.build(cloudProvenance(context, request, jsonStringOr(j, "speech_model_used", m_spec.id), {}));
            if (!transcript) {
                return std::unexpected(transcript.error());
            }
            TranscribeResult result;
            result.transcript = std::move(*transcript);
            const double duration = jsonNumberOr(j, "audio_duration", 0.0);
            // At most a day of audio: a larger or negative duration is not believable and must not overflow.
            const std::int64_t seconds = duration > 0.0 && duration <= 86400.0 ? static_cast<std::int64_t>(std::ceil(duration)) : 0;
            result.duration = Ticks::fromSeconds(seconds);
            result.usage.audioMilliseconds = seconds * 1000;
            result.requestId = id;
            report(1.0f);
            return result;
        });
    }

private:
    ProviderSpec m_provider;
    ModelSpec m_spec;
    std::string m_baseUrl;
    Secret m_key;
    IHttpTransport& m_transport;
    AdapterOptions m_options;
};

} // namespace

std::unique_ptr<ITranscriber> makeDeepgramTranscriber(ProviderContext context) {
    return std::make_unique<DeepgramTranscriber>(std::move(context));
}

std::unique_ptr<ITranscriber> makeElevenLabsTranscriber(ProviderContext context) {
    return std::make_unique<ElevenLabsTranscriber>(std::move(context));
}

std::unique_ptr<ITranscriber> makeAssemblyAiTranscriber(ProviderContext context, AdapterOptions options) {
    return std::make_unique<AssemblyAiTranscriber>(std::move(context), std::move(options));
}

void registerCloudAdapters(ProviderRegistry& registry, AdapterOptions options) {
    registry.registerMediaGenerator("gemini-veo", makeVeoGenerator);
    registry.registerMediaGenerator("runway-generation", makeRunwayGenerator);
    registry.registerChat("anthropic-messages", [](ProviderContext c) { return makeAnthropicChat(std::move(c)); });
    registry.registerChat("openai-responses", [](ProviderContext c) { return makeOpenAiResponsesChat(std::move(c)); });
    registry.registerChat("chat-completions", [](ProviderContext c) { return makeChatCompletionsChat(std::move(c)); });
    registry.registerChat("gemini-interactions",
                          [options](ProviderContext c) { return makeGeminiInteractionsChat(std::move(c), options); });
    registry.registerTranscriber("deepgram-listen", [](ProviderContext c) { return makeDeepgramTranscriber(std::move(c)); });
    registry.registerTranscriber("elevenlabs-stt", [](ProviderContext c) { return makeElevenLabsTranscriber(std::move(c)); });
    registry.registerSpeechSynth("elevenlabs-tts", [](ProviderContext c) { return makeElevenLabsSpeech(std::move(c)); });
    registry.registerSpeechSynth("openai-speech", [](ProviderContext c) { return makeOpenAiSpeech(std::move(c)); });
    registry.registerSpeechSynth("gemini-tts", [](ProviderContext c) { return makeGeminiSpeech(std::move(c)); });
    registry.registerTranscriber("assemblyai",
                                 [options](ProviderContext c) { return makeAssemblyAiTranscriber(std::move(c), options); });
}

} // namespace wabcut::ai::net
