// Speech-to-text vendors with their own job APIs (r07, docs/ai.md "Providers"), checked on 2026-11-03:
//   speechmatics  {base = https://<region>.asr.api.speechmatics.com/v2}, Authorization: Bearer.
//                 POST /jobs multipart "config" (JobConfig JSON: {"type": "transcription", "transcription_config":
//                 {"language": "en" | "auto", "model": "enhanced" | "standard", "diarization": "speaker" | "none"}}) +
//                 "data_file" -> {"id"}; GET /jobs/{id} -> {"job": {"status": running | done | rejected | deleted | expired,
//                 "duration", "errors"}}; GET /jobs/{id}/transcript?format=json-v2 -> {"results": [{"type": word |
//                 punctuation, "start_time", "end_time", "attaches_to", "alternatives": [{"content", "confidence",
//                 "language", "speaker" ("S1", "UU" = unknown)}]}]}; DELETE /jobs/{id}?force=true.
//                 docs.speechmatics.com/api-ref/batch/create-a-new-job, .../speech-to-text/batch/output
//   gladia        {base = https://api.gladia.io}, x-gladia-key. POST /v2/upload multipart "audio" -> {"audio_url"};
//                 POST /v2/pre-recorded {"audio_url", "diarization", "diarization_config", "language_config":
//                 {"languages", "code_switching"}} -> {"id", "result_url"}; GET /v2/pre-recorded/{id} -> {"status":
//                 queued | processing | done | error, "result": {"metadata": {"audio_duration", "billing_time"},
//                 "transcription": {"languages", "utterances": [{"start", "end", "speaker", "words": [{"word",
//                 "start", "end", "confidence"}]}]}}}; DELETE /v2/pre-recorded/{id}. docs.gladia.io/api-reference/v2
// Both keep the audio on the vendor until it is deleted: the adapters delete the job once the transcript is read (and
// when a run is cancelled), best effort. Times are seconds.

#include "AdapterUtil.h"

#include <wabcut/ai/JsonAccess.h>
#include <wabcut/ai_net/Adapters.h>
#include <wabcut/model/MediaLocator.h>

#include <algorithm>
#include <cmath>

namespace wabcut::ai::net {

namespace {

Ticks secondsAt(const Json& j, const char* key) {
    const auto value = jsonNumber(j, key);
    return value ? ticksFromSeconds(*value).value_or(Ticks{}) : Ticks{};
}

float confidence(const Json& j) {
    const double c = jsonNumberOr(j, "confidence", 0.0);
    return c >= 0.0 && c <= 1.0 ? static_cast<float>(c) : 0.0f;
}

std::string trimmed(std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::expected<void, AiError> checkFile(const TranscribeRequest& request, const ModelSpec& spec) {
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
    return {};
}

/// The common shape of both: upload, then poll a job until it ends; delete it afterwards.
class JobTranscriber : public ITranscriber {
public:
    JobTranscriber(ProviderContext context, AdapterOptions options)
        : m_provider(context.provider), m_spec(context.model), m_baseUrl(std::move(context.baseUrl)), m_key(std::move(context.key)),
          m_transport(context.transport), m_options(std::move(options)) {}

    const ModelSpec& spec() const override { return m_spec; }

protected:
    ProviderContext context() const { return ProviderContext{m_provider, m_spec, m_baseUrl, m_key.clone(), m_transport}; }

    /// Best effort, outside the run's stop token: the audio should not stay on the vendor.
    void deleteJob(const std::string& url) {
        HttpRequest http;
        http.method = "DELETE";
        http.url = url;
        http.headers = providerHeaders(context());
        http.timeout = std::chrono::seconds(10);
        http.idleTimeout = std::chrono::seconds(10);
        http.retrySafe = false;
        (void)m_transport.send(http, {}, std::stop_token{});
    }

    /// Waits one poll interval; false when cancelled.
    bool wait(std::stop_token stop) { return m_options.sleeper(m_options.pollInterval, stop); }

    ProviderSpec m_provider;
    ModelSpec m_spec;
    std::string m_baseUrl;
    Secret m_key;
    IHttpTransport& m_transport;
    AdapterOptions m_options;
};

// --- Speechmatics ---------------------------------------------------------------------------------------------------

class SpeechmaticsTranscriber final : public JobTranscriber {
public:
    using JobTranscriber::JobTranscriber;

    std::expected<TranscribeResult, AiError> transcribe(const TranscribeRequest& request, const ProgressFn& progress,
                                                        std::stop_token stop) override {
        if (auto ok = checkFile(request, m_spec); !ok) {
            return std::unexpected(ok.error());
        }
        const auto report = [&](float value) {
            if (progress) {
                progress(value);
            }
        };
        Json transcription{{"language", request.language.empty() ? std::string("auto") : normalizeLanguage(request.language)},
                           {"model", m_spec.id},
                           {"diarization", request.diarize ? "speaker" : "none"}};
        if (!request.keyterms.empty()) {
            Json vocabulary = Json::array();
            for (const auto& term : request.keyterms) {
                vocabulary.push_back(Json{{"content", term}});
            }
            transcription["additional_vocab"] = std::move(vocabulary);
        }
        const Json config{{"type", "transcription"}, {"transcription_config", std::move(transcription)}};
        const ProviderContext c = context();
        HttpRequest submit;
        submit.method = "POST";
        submit.url = joinUrl(m_baseUrl, "/jobs");
        submit.headers = providerHeaders(c);
        submit.multipart.push_back({"config", config.dump(), {}, {}, {}});
        submit.multipart.push_back({"data_file", {}, request.audioFile, pathToUtf8(request.audioFile.filename()), request.mediaType});
        submit.retrySafe = false;
        submit.timeout = std::chrono::hours(1);
        HttpStreamHandlers handlers;
        handlers.onUploadProgress = [&](std::uint64_t sent, std::uint64_t total) {
            report(total ? 0.4f * static_cast<float>(sent) / static_cast<float>(total) : 0.0f);
        };
        auto created = m_transport.send(submit, handlers, stop);
        if (!created) {
            return std::unexpected(created.error());
        }
        if (!created->ok()) {
            return std::unexpected(httpError(*created));
        }
        auto job = parseJsonBody(created->body, "Speechmatics job");
        if (!job) {
            return std::unexpected(job.error());
        }
        const std::string id = jsonIdOr(*job, "id");
        if (!safePathId(id)) {
            return std::unexpected(AiError(AiErrorCode::ProviderError, "Speechmatics returned no valid job id"));
        }
        const std::string jobUrl = joinUrl(m_baseUrl, "/jobs/" + id);
        report(0.45f);

        double duration = 0.0;
        const auto deadline = std::chrono::steady_clock::now() + m_options.maxPollTime;
        for (;;) {
            if (!wait(stop)) {
                deleteJob(jobUrl + "?force=true");
                return std::unexpected(AiError(AiErrorCode::Cancelled, "cancelled while Speechmatics was transcribing"));
            }
            auto details = getJson(m_transport, c, jobUrl, "Speechmatics job", stop);
            if (!details) {
                if (details.error().code == AiErrorCode::Cancelled) {
                    deleteJob(jobUrl + "?force=true");
                }
                return std::unexpected(details.error());
            }
            const Json none = Json::object();
            const Json* jobJson = jsonObject(*details, "job");
            const Json& state = jobJson ? *jobJson : none;
            const std::string status = jsonStringOr(state, "status");
            duration = jsonNumberOr(state, "duration", duration);
            if (status == "done") {
                break;
            }
            if (status == "rejected" || status == "deleted" || status == "expired") {
                std::string reason = "Speechmatics " + status + " the job";
                if (const Json* errors = jsonArray(state, "errors"); errors && !errors->empty() && (*errors)[0].is_object()) {
                    reason += ": " + jsonStringOr((*errors)[0], "message");
                }
                return std::unexpected(AiError(AiErrorCode::ProviderError, reason));
            }
            if (status != "running" && !status.empty()) {
                return std::unexpected(malformedEvent("Speechmatics job status"));
            }
            report(0.7f);
            if (std::chrono::steady_clock::now() > deadline) {
                deleteJob(jobUrl + "?force=true");
                return std::unexpected(AiError(AiErrorCode::Network, "Speechmatics did not finish before the deadline"));
            }
        }
        auto transcriptJson = getJson(m_transport, c, jobUrl + "/transcript?format=json-v2", "Speechmatics transcript", stop);
        deleteJob(jobUrl);
        if (!transcriptJson) {
            return std::unexpected(transcriptJson.error());
        }
        const Json& j = *transcriptJson;
        try {
            TranscriptBuilder builder(request);
            std::string language = stringAt(j, {"metadata", "transcription_config", "language"});
            // A word with the punctuation that attaches to it, added once the next word starts.
            struct Pending {
                std::string text;
                Ticks t0{}, t1{};
                float confidence = 0.0f;
                std::string speaker;
            };
            std::optional<Pending> pending;
            std::string prefix; // punctuation that attaches to the next word
            const auto flush = [&] {
                if (pending) {
                    builder.addWord(std::move(pending->text), pending->t0, pending->t1, pending->confidence, pending->speaker);
                    pending.reset();
                }
            };
            if (const Json* results = jsonArray(j, "results")) {
                for (const Json& r : *results) {
                    const Json* alternatives = jsonArray(r, "alternatives");
                    if (!alternatives || alternatives->empty() || !(*alternatives)[0].is_object()) {
                        continue;
                    }
                    const Json& best = (*alternatives)[0];
                    const std::string content = jsonStringOr(best, "content");
                    const std::string type = jsonStringOr(r, "type");
                    if (type == "punctuation") {
                        const std::string attaches = jsonStringOr(r, "attaches_to", "previous");
                        if ((attaches == "previous" || attaches == "both") && pending) {
                            pending->text += content;
                        } else if (attaches == "next") {
                            prefix += content;
                        }
                        continue;
                    }
                    if (type != "word" && type != "entity") {
                        continue;
                    }
                    flush();
                    std::string speaker = jsonStringOr(best, "speaker");
                    if (speaker == "UU") {
                        speaker.clear();
                    }
                    if (language.empty() || language == "auto") {
                        language = jsonStringOr(best, "language", language);
                    }
                    pending = Pending{prefix + content, secondsAt(r, "start_time"), secondsAt(r, "end_time"), confidence(best), std::move(speaker)};
                    prefix.clear();
                }
            }
            flush();
            if (language != "auto") {
                builder.setLanguage(language);
            }
            auto transcript = builder.build(cloudProvenance(c, request, m_spec.id, {}));
            if (!transcript) {
                return std::unexpected(transcript.error());
            }
            TranscribeResult result;
            result.transcript = std::move(*transcript);
            const double seconds = duration > 0.0 && duration <= 86400.0 ? duration : static_cast<double>(request.audioMilliseconds) / 1000.0;
            result.duration = ticksFromSeconds(seconds).value_or(Ticks{});
            result.usage.audioMilliseconds = static_cast<std::int64_t>(std::ceil(seconds * 1000.0));
            result.requestId = id;
            report(1.0f);
            return result;
        } catch (const std::exception& e) {
            return std::unexpected(AiError(AiErrorCode::ProviderError, std::string("unreadable Speechmatics response: ") + e.what()));
        }
    }
};

// --- Gladia ---------------------------------------------------------------------------------------------------------

class GladiaTranscriber final : public JobTranscriber {
public:
    using JobTranscriber::JobTranscriber;

    std::expected<TranscribeResult, AiError> transcribe(const TranscribeRequest& request, const ProgressFn& progress,
                                                        std::stop_token stop) override {
        if (auto ok = checkFile(request, m_spec); !ok) {
            return std::unexpected(ok.error());
        }
        const auto report = [&](float value) {
            if (progress) {
                progress(value);
            }
        };
        const ProviderContext c = context();
        // 1. Upload.
        HttpRequest upload;
        upload.method = "POST";
        upload.url = joinUrl(m_baseUrl, "/v2/upload");
        upload.headers = providerHeaders(c);
        upload.multipart.push_back({"audio", {}, request.audioFile, pathToUtf8(request.audioFile.filename()), request.mediaType});
        upload.timeout = std::chrono::hours(1);
        HttpStreamHandlers handlers;
        handlers.onUploadProgress = [&](std::uint64_t sent, std::uint64_t total) {
            report(total ? 0.4f * static_cast<float>(sent) / static_cast<float>(total) : 0.0f);
        };
        auto uploaded = m_transport.send(upload, handlers, stop);
        if (!uploaded) {
            return std::unexpected(uploaded.error());
        }
        if (!uploaded->ok()) {
            return std::unexpected(httpError(*uploaded));
        }
        auto uploadJson = parseJsonBody(uploaded->body, "Gladia upload");
        if (!uploadJson) {
            return std::unexpected(uploadJson.error());
        }
        const std::string audioUrl = jsonStringOr(*uploadJson, "audio_url");
        if (audioUrl.empty()) {
            return std::unexpected(AiError(AiErrorCode::ProviderError, "Gladia returned no audio_url"));
        }
        // 2. Submit.
        Json body{{"audio_url", audioUrl}, {"diarization", request.diarize}};
        if (request.diarize && request.speakerCount) {
            body["diarization_config"] = Json{{"number_of_speakers", *request.speakerCount}};
        }
        body["language_config"] = Json{{"languages", request.language.empty() ? Json::array() : Json::array({normalizeLanguage(request.language)})},
                                       {"code_switching", false}};
        if (!request.keyterms.empty()) {
            body["custom_vocabulary"] = true;
            body["custom_vocabulary_config"] = Json{{"vocabulary", request.keyterms}};
        }
        HttpRequest submit = jsonRequest(c, endpointUrl(c), body, BodyMode::Buffered);
        submit.retrySafe = false;
        auto job = sendForJson(m_transport, submit, "Gladia job", stop);
        if (!job) {
            return std::unexpected(job.error());
        }
        const std::string id = jsonIdOr(*job, "id");
        if (!safePathId(id)) {
            return std::unexpected(AiError(AiErrorCode::ProviderError, "Gladia returned no valid job id"));
        }
        const std::string jobUrl = joinUrl(m_baseUrl, "/v2/pre-recorded/" + id);
        report(0.45f);
        // 3. Poll.
        Json done;
        const auto deadline = std::chrono::steady_clock::now() + m_options.maxPollTime;
        for (;;) {
            if (!wait(stop)) {
                deleteJob(jobUrl);
                return std::unexpected(AiError(AiErrorCode::Cancelled, "cancelled while Gladia was transcribing"));
            }
            auto polled = getJson(m_transport, c, jobUrl, "Gladia job", stop);
            if (!polled) {
                if (polled.error().code == AiErrorCode::Cancelled) {
                    deleteJob(jobUrl);
                }
                return std::unexpected(polled.error());
            }
            const std::string status = jsonStringOr(*polled, "status");
            if (status == "done") {
                done = std::move(*polled);
                break;
            }
            if (status == "error") {
                deleteJob(jobUrl);
                return std::unexpected(AiError(AiErrorCode::ProviderError, "Gladia could not transcribe the audio (" +
                                                                               jsonIdOr(*polled, "error_code") + ")"));
            }
            if (status != "queued" && status != "processing") {
                return std::unexpected(malformedEvent("Gladia job status"));
            }
            report(status == "processing" ? 0.7f : 0.5f);
            if (std::chrono::steady_clock::now() > deadline) {
                deleteJob(jobUrl);
                return std::unexpected(AiError(AiErrorCode::Network, "Gladia did not finish before the deadline"));
            }
        }
        deleteJob(jobUrl);
        // 4. Words.
        try {
            const Json& j = done;
            TranscriptBuilder builder(request);
            const Json* transcription = jsonAt(j, {"result", "transcription"});
            const Json none = Json::object();
            const Json& t = transcription && transcription->is_object() ? *transcription : none;
            if (const Json* languages = jsonArray(t, "languages"); languages && !languages->empty()) {
                builder.setLanguage(asString(&(*languages)[0]).value_or(""));
            }
            if (const Json* utterances = jsonArray(t, "utterances")) {
                for (const Json& u : *utterances) {
                    const std::string speaker = jsonIdOr(u, "speaker");
                    if (const Json* words = jsonArray(u, "words")) {
                        for (const Json& w : *words) {
                            builder.addWord(trimmed(jsonStringOr(w, "word")), secondsAt(w, "start"), secondsAt(w, "end"), confidence(w),
                                            speaker);
                        }
                    }
                    builder.addUtterance(secondsAt(u, "start"), secondsAt(u, "end"));
                }
            }
            auto transcript = builder.build(cloudProvenance(c, request, m_spec.id, {}));
            if (!transcript) {
                return std::unexpected(transcript.error());
            }
            TranscribeResult result;
            result.transcript = std::move(*transcript);
            const Json* metadata = jsonAt(j, {"result", "metadata"});
            const double audio = metadata ? jsonNumberOr(*metadata, "audio_duration", 0.0) : 0.0;
            const double billed = metadata ? jsonNumberOr(*metadata, "billing_time", audio) : audio;
            const double seconds = audio > 0.0 && audio <= 86400.0 ? audio : static_cast<double>(request.audioMilliseconds) / 1000.0;
            result.duration = ticksFromSeconds(seconds).value_or(Ticks{});
            const double charged = billed > 0.0 && billed <= 86400.0 ? billed : seconds;
            result.usage.audioMilliseconds = static_cast<std::int64_t>(std::ceil(charged * 1000.0));
            result.requestId = id;
            report(1.0f);
            return result;
        } catch (const std::exception& e) {
            return std::unexpected(AiError(AiErrorCode::ProviderError, std::string("unreadable Gladia response: ") + e.what()));
        }
    }
};

} // namespace

std::unique_ptr<ITranscriber> makeSpeechmaticsTranscriber(ProviderContext context, AdapterOptions options) {
    return std::make_unique<SpeechmaticsTranscriber>(std::move(context), std::move(options));
}

std::unique_ptr<ITranscriber> makeGladiaTranscriber(ProviderContext context, AdapterOptions options) {
    return std::make_unique<GladiaTranscriber>(std::move(context), std::move(options));
}

} // namespace wabcut::ai::net
