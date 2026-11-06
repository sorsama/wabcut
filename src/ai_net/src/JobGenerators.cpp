// Generation adapters that run a job on the provider (r07, docs/ai.md "Providers"), checked on 2026-11-03:
//   fal-queue      POST https://queue.fal.run/{model id}, Authorization: Key <key>, the model's input JSON -> {"request_id",
//                  "status_url", "response_url", "cancel_url"} (queue.fal.run only); GET status_url -> {"status": IN_QUEUE |
//                  IN_PROGRESS | COMPLETED, "error"?}; GET response_url -> the model's output ("images": [{"url"}],
//                  "video": {"url"}, "audio": {"url"}); PUT cancel_url. Outputs on fal.media / *.fal.media, public.
//                  fal.ai/docs/model-apis/model-endpoints/queue
//   replicate      POST {base}/models/{owner}/{name}/predictions, bearer, {"input": {...}} -> {"id", "status", "urls":
//                  {"get", "cancel"}}; GET urls.get -> {"status": starting | processing | succeeded | failed | canceled,
//                  "output": URL | [URL], "error"}; POST urls.cancel. Outputs on replicate.delivery / *.replicate.delivery,
//                  fetched with the key (the docs ask for it). replicate.com/docs/reference/http
//   xai-video      POST {base}/videos/generations, bearer, {"model", "prompt", "duration", "aspect_ratio", "resolution",
//                  "generate_audio"} -> {"request_id"}; GET {base}/videos/{id} -> {"status": pending | done | failed |
//                  expired, "video": {"url"} (vidgen.x.ai, public), "error"}. docs.x.ai/developers/model-capabilities/video/generation
//   openai-videos  POST {base}{endpoint = /videos} {"model", "prompt", "seconds", "size"} -> {"id", "status"}; GET
//                  /videos/{id} -> {"status": queued | in_progress | completed | failed, "progress", "error"}; GET
//                  /videos/{id}/content with the key (the provider's own host). OpenAI's own Sora API was shut down on
//                  2026-09-24: this serves OpenAI-compatible custom providers.
//                  developers.openai.com/api/reference/resources/videos
// Submissions are never retried: a network ambiguity must not bill a job twice.

#include "GeneratorBase.h"

#include <wabcut/ai_net/Adapters.h>

#include <algorithm>
#include <cmath>

namespace wabcut::ai::net {

namespace {

AiError invalid(std::string text) { return {AiErrorCode::InvalidRequest, std::move(text)}; }

/// An absolute HTTPS URL on one of `hosts` (port 443, no userinfo): where a provider's job URLs may point.
bool onHosts(std::string_view url, std::initializer_list<std::string_view> hosts) {
    const auto parts = parseHttpUrl(url);
    return parts && parts->scheme == "https" && parts->userinfo.empty() && (!parts->port || *parts->port == 443) &&
           hostMatches(parts->host, hosts);
}

/// The conditioning image as a data URL (fal, Replicate and OpenAI-compatible APIs take them).
std::expected<std::string, AiError> imageDataUrl(const std::filesystem::path& path) {
    auto image = loadConditioningImage(path, 3u * 1024 * 1024);
    if (!image) {
        return std::unexpected(image.error());
    }
    return dataUrl(*image);
}

GenerationStatus failedWith(std::string message, std::string code = {}) {
    GenerationStatus s;
    s.state = GenerationState::Failed;
    s.failure = std::move(message);
    s.failureCode = std::move(code);
    return s;
}

/// Best-effort cancel request with short timeouts (it may run while the editor shuts down).
std::expected<void, AiError> sendCancel(IHttpTransport& http, HttpRequest request, std::stop_token stop) {
    request.timeout = std::chrono::seconds(5);
    request.idleTimeout = std::chrono::seconds(5);
    request.retrySafe = false;
    auto result = http.send(request, {}, stop);
    if (!result) {
        return std::unexpected(result.error());
    }
    if (!result->ok() && result->status != 400) { // 400 = already finished (fal)
        return std::unexpected(httpError(*result));
    }
    return {};
}

std::string falImageSize(std::string_view aspect) {
    if (aspect == "16:9") {
        return "landscape_16_9";
    }
    if (aspect == "9:16") {
        return "portrait_16_9";
    }
    if (aspect == "4:3") {
        return "landscape_4_3";
    }
    if (aspect == "3:4") {
        return "portrait_4_3";
    }
    return "square_hd";
}

// --- fal.ai queue ------------------------------------------------------------------------------------------------------

class FalGenerator final : public GeneratorBase {
public:
    using GeneratorBase::GeneratorBase;

    std::expected<void, AiError> cancel(const GenerationTicket& ticket, std::stop_token stop) override {
        if (!onHosts(ticket.cancelUrl, {"queue.fal.run"})) {
            return std::unexpected(invalid("invalid generation ticket"));
        }
        HttpRequest http;
        http.method = "PUT";
        http.url = ticket.cancelUrl;
        http.headers = providerHeaders(context());
        return sendCancel(m_http, std::move(http), stop);
    }

protected:
    std::expected<Submitted, AiError> submit(const MediaGenerationRequest& r, std::stop_token stop) override {
        if (!safePathId(m_model.id, true)) {
            return std::unexpected(invalid("not a fal.ai model id"));
        }
        auto input = inputFor(r);
        if (!input) {
            return std::unexpected(input.error());
        }
        auto j = postJson(joinUrl(m_baseUrl, "/" + m_model.id), *input, "fal.ai", stop);
        if (!j) {
            return std::unexpected(j.error());
        }
        GenerationTicket ticket;
        ticket.id = jsonStringOr(*j, "request_id");
        ticket.pollUrl = jsonStringOr(*j, "status_url");
        ticket.resultUrl = jsonStringOr(*j, "response_url");
        ticket.cancelUrl = jsonStringOr(*j, "cancel_url");
        if (!safePathId(ticket.id) || !onHosts(ticket.pollUrl, {"queue.fal.run"}) || !onHosts(ticket.resultUrl, {"queue.fal.run"}) ||
            (!ticket.cancelUrl.empty() && !onHosts(ticket.cancelUrl, {"queue.fal.run"}))) {
            return std::unexpected(malformedEvent("fal.ai queue answer"));
        }
        return Submitted{std::nullopt, std::move(ticket)};
    }

    std::expected<GenerationStatus, AiError> status(const GenerationTicket& ticket, std::stop_token stop) override {
        if (!onHosts(ticket.pollUrl, {"queue.fal.run"}) || !onHosts(ticket.resultUrl, {"queue.fal.run"})) {
            return std::unexpected(invalid("invalid generation ticket"));
        }
        auto j = getJson(m_http, context(), ticket.pollUrl, "fal.ai status", stop);
        if (!j) {
            return std::unexpected(j.error());
        }
        const std::string state = jsonStringOr(*j, "status");
        GenerationStatus s;
        if (state == "IN_QUEUE") {
            s.state = GenerationState::Queued;
            return s;
        }
        if (state == "IN_PROGRESS") {
            s.state = GenerationState::Running;
            return s;
        }
        if (state != "COMPLETED") {
            return std::unexpected(malformedEvent("fal.ai status"));
        }
        if (const std::string error = jsonIdOr(*j, "error"); !error.empty()) {
            return failedWith("fal.ai: " + error, jsonStringOr(*j, "error_type"));
        }
        auto result = getJson(m_http, context(), ticket.resultUrl, "fal.ai result", stop);
        if (!result) {
            if (result.error().httpStatus == 422) { // the model refused the input (e.g. its safety checker)
                return failedWith(result.error().message, "invalid");
            }
            return std::unexpected(result.error());
        }
        s.state = GenerationState::Succeeded;
        s.progress = 1.0f;
        if (const Json* images = jsonArray(*result, "images")) {
            for (const Json& image : *images) {
                if (image.is_object() && !jsonStringOr(image, "url").empty()) {
                    s.outputUrls.push_back(jsonStringOr(image, "url"));
                }
            }
            s.usage.images = static_cast<std::int64_t>(s.outputUrls.size());
        }
        for (const char* key : {"video", "audio", "audio_file", "image"}) {
            if (const Json* file = jsonObject(*result, key); file && !jsonStringOr(*file, "url").empty()) {
                s.outputUrls.push_back(jsonStringOr(*file, "url"));
            }
        }
        if (s.outputUrls.empty()) {
            return failedWith("fal.ai returned no output (possibly filtered)", "content_policy");
        }
        return s;
    }

    std::expected<std::filesystem::path, AiError> download(const std::string& url, const std::filesystem::path& directory,
                                                           std::stop_token stop) override {
        return downloadGenerated(m_http, url, {}, directory, {"fal.media", "*.fal.media"}, m_kind, stop);
    }

private:
    /// The model's input: the families Wabcut lists have their own field names; other fal models get the common ones.
    std::expected<Json, AiError> inputFor(const MediaGenerationRequest& r) const {
        const std::string& id = m_model.id;
        Json in = Json::object();
        if (r.kind == GeneratedMediaKind::SoundEffect) {
            const double seconds = soundEffectSeconds(m_model, r);
            if (id.find("elevenlabs/sound-effects") != std::string::npos) {
                in["text"] = r.prompt;
                if (r.duration) {
                    in["duration_seconds"] = seconds;
                }
                if (r.promptInfluence) {
                    in["prompt_influence"] = *r.promptInfluence;
                }
                if (r.loop) {
                    in["loop"] = true;
                }
            } else {
                in["prompt"] = r.prompt;
                in["duration"] = static_cast<int>(std::ceil(seconds));
                if (!r.negativePrompt.empty()) {
                    in["negative_prompt"] = r.negativePrompt;
                }
            }
        } else if (r.kind == GeneratedMediaKind::Image) {
            in["prompt"] = r.prompt;
            in["image_size"] = falImageSize(r.aspectRatio);
            in["output_format"] = "png";
            if (r.count > 1) {
                in["num_images"] = r.count;
            }
        } else { // video
            in["prompt"] = r.prompt;
            const int seconds = generatedSeconds(m_model.generation, r.duration);
            if (seconds > 0) {
                in["duration"] = std::to_string(seconds);
            }
            if (!r.aspectRatio.empty()) {
                in["aspect_ratio"] = r.aspectRatio;
            }
            if (m_model.generation.audio) {
                in["generate_audio"] = r.audio;
            }
            if (!r.negativePrompt.empty()) {
                in["negative_prompt"] = r.negativePrompt;
            }
            const bool kling = id.find("kling-video") != std::string::npos;
            if (!r.firstFrame.empty()) {
                auto url = imageDataUrl(r.firstFrame);
                if (!url) {
                    return std::unexpected(url.error());
                }
                in[kling ? "start_image_url" : "image_url"] = *url;
            }
            if (!r.lastFrame.empty()) {
                auto url = imageDataUrl(r.lastFrame);
                if (!url) {
                    return std::unexpected(url.error());
                }
                in["end_image_url"] = *url;
            }
        }
        if (r.seed && r.kind != GeneratedMediaKind::Video) {
            in["seed"] = *r.seed;
        }
        return in;
    }
};

// --- Replicate ---------------------------------------------------------------------------------------------------------

class ReplicateGenerator final : public GeneratorBase {
public:
    using GeneratorBase::GeneratorBase;

    std::expected<void, AiError> cancel(const GenerationTicket& ticket, std::stop_token stop) override {
        if (!safePathId(ticket.id)) {
            return std::unexpected(invalid("invalid generation ticket"));
        }
        HttpRequest http;
        http.method = "POST";
        http.url = joinUrl(m_baseUrl, "/predictions/" + ticket.id + "/cancel");
        http.headers = providerHeaders(context());
        return sendCancel(m_http, std::move(http), stop);
    }

protected:
    std::expected<Submitted, AiError> submit(const MediaGenerationRequest& r, std::stop_token stop) override {
        if (!safePathId(m_model.id, true) || std::count(m_model.id.begin(), m_model.id.end(), '/') != 1) {
            return std::unexpected(invalid("not a Replicate model id (owner/name)"));
        }
        Json input{{"prompt", r.prompt}};
        if (!r.aspectRatio.empty()) {
            input["aspect_ratio"] = r.aspectRatio;
        }
        if (r.seed) {
            input["seed"] = *r.seed;
        }
        if (r.kind == GeneratedMediaKind::Image) {
            input["output_format"] = "png";
            if (m_model.id.ends_with("flux-schnell") || m_model.id.ends_with("flux-dev")) {
                input["num_outputs"] = r.count;
            } else if (r.count != 1) {
                return std::unexpected(invalid("one image per run on this model"));
            }
        } else {
            const int seconds = generatedSeconds(m_model.generation, r.duration);
            if (seconds > 0) {
                input["duration"] = seconds;
            }
            if (m_model.generation.audio) {
                input["generate_audio"] = r.audio;
            }
            if (!r.negativePrompt.empty()) {
                input["negative_prompt"] = r.negativePrompt;
            }
            if (!r.firstFrame.empty()) {
                auto url = imageDataUrl(r.firstFrame);
                if (!url) {
                    return std::unexpected(url.error());
                }
                input["image"] = *url;
            }
            if (!r.lastFrame.empty()) {
                auto url = imageDataUrl(r.lastFrame);
                if (!url) {
                    return std::unexpected(url.error());
                }
                input["last_frame"] = *url;
            }
        }
        auto j = postJson(joinUrl(m_baseUrl, "/models/" + m_model.id + "/predictions"), Json{{"input", input}}, "Replicate", stop);
        if (!j) {
            return std::unexpected(j.error());
        }
        GenerationTicket ticket;
        ticket.id = jsonStringOr(*j, "id");
        if (!safePathId(ticket.id)) {
            return std::unexpected(malformedEvent("Replicate prediction"));
        }
        return Submitted{std::nullopt, std::move(ticket)};
    }

    std::expected<GenerationStatus, AiError> status(const GenerationTicket& ticket, std::stop_token stop) override {
        if (!safePathId(ticket.id)) {
            return std::unexpected(invalid("invalid generation ticket"));
        }
        auto j = getJson(m_http, context(), joinUrl(m_baseUrl, "/predictions/" + ticket.id), "Replicate prediction", stop);
        if (!j) {
            return std::unexpected(j.error());
        }
        const std::string state = jsonStringOr(*j, "status");
        GenerationStatus s;
        if (state == "starting") {
            s.state = GenerationState::Queued;
        } else if (state == "processing") {
            s.state = GenerationState::Running;
        } else if (state == "canceled") {
            s.state = GenerationState::Cancelled;
        } else if (state == "failed") {
            const std::string error = jsonIdOr(*j, "error");
            s = failedWith("Replicate: " + (error.empty() ? std::string("the prediction failed") : error),
                           error.find("NSFW") != std::string::npos || error.find("safety") != std::string::npos ? "content_policy" : "");
        } else if (state == "succeeded") {
            s.state = GenerationState::Succeeded;
            s.progress = 1.0f;
            const Json* output = jsonMember(*j, "output");
            if (output && output->is_string()) {
                s.outputUrls.push_back(output->get<std::string>());
            } else if (output && output->is_array()) {
                for (const Json& item : *output) {
                    if (item.is_string()) {
                        s.outputUrls.push_back(item.get<std::string>());
                    }
                }
            }
            if (s.outputUrls.empty()) {
                return failedWith("Replicate returned no output", "content_policy");
            }
            if (m_kind == GeneratedMediaKind::Image) {
                s.usage.images = static_cast<std::int64_t>(s.outputUrls.size());
            }
        } else {
            return std::unexpected(malformedEvent("Replicate status"));
        }
        return s;
    }

    std::expected<std::filesystem::path, AiError> download(const std::string& url, const std::filesystem::path& directory,
                                                           std::stop_token stop) override {
        // Replicate's own delivery hosts, which want the key (replicate.com/docs/reference/http, "output files").
        return downloadGenerated(m_http, url, providerHeaders(context()), directory, {"replicate.delivery", "*.replicate.delivery"}, m_kind,
                                 stop);
    }
};

// --- xAI video ---------------------------------------------------------------------------------------------------------

class XaiVideo final : public GeneratorBase {
public:
    using GeneratorBase::GeneratorBase;

protected:
    std::expected<Submitted, AiError> submit(const MediaGenerationRequest& r, std::stop_token stop) override {
        Json body{{"model", m_model.id}, {"prompt", r.prompt}};
        const int seconds = generatedSeconds(m_model.generation, r.duration);
        if (seconds > 0) {
            body["duration"] = seconds;
        }
        if (!r.aspectRatio.empty()) {
            body["aspect_ratio"] = r.aspectRatio;
        }
        if (!r.resolution.empty()) {
            body["resolution"] = r.resolution;
        }
        body["generate_audio"] = r.audio;
        if (!r.firstFrame.empty()) {
            auto url = imageDataUrl(r.firstFrame);
            if (!url) {
                return std::unexpected(url.error());
            }
            body["image"] = Json{{"url", *url}};
        }
        auto j = postJson(endpointUrl(context()), body, "xAI", stop);
        if (!j) {
            return std::unexpected(j.error());
        }
        GenerationTicket ticket;
        ticket.id = jsonStringOr(*j, "request_id");
        if (!safePathId(ticket.id)) {
            return std::unexpected(malformedEvent("xAI video job"));
        }
        return Submitted{std::nullopt, std::move(ticket)};
    }

    std::expected<GenerationStatus, AiError> status(const GenerationTicket& ticket, std::stop_token stop) override {
        if (!safePathId(ticket.id)) {
            return std::unexpected(invalid("invalid generation ticket"));
        }
        auto j = getJson(m_http, context(), joinUrl(m_baseUrl, "/videos/" + ticket.id), "xAI video", stop);
        if (!j) {
            return std::unexpected(j.error());
        }
        const std::string state = jsonStringOr(*j, "status");
        GenerationStatus s;
        if (state == "pending") {
            s.state = GenerationState::Running;
        } else if (state == "done") {
            const std::string url = stringAt(*j, {"video", "url"});
            if (url.empty()) {
                return std::unexpected(malformedEvent("xAI video result"));
            }
            s.state = GenerationState::Succeeded;
            s.progress = 1.0f;
            s.outputUrls.push_back(url);
        } else if (state == "failed" || state == "expired") {
            const std::string code = stringAt(*j, {"error", "code"});
            s = failedWith("xAI: " + (state == "expired" ? std::string("the job expired") : stringAt(*j, {"error", "message"})),
                           code == "invalid_argument" || code == "permission_denied" ? "invalid" : code);
        } else {
            return std::unexpected(malformedEvent("xAI video status"));
        }
        return s;
    }

    std::expected<std::filesystem::path, AiError> download(const std::string& url, const std::filesystem::path& directory,
                                                           std::stop_token stop) override {
        return downloadGenerated(m_http, url, {}, directory, {"vidgen.x.ai"}, GeneratedMediaKind::Video, stop);
    }
};

// --- OpenAI-compatible /videos -----------------------------------------------------------------------------------------

class OpenAiVideos final : public GeneratorBase {
public:
    using GeneratorBase::GeneratorBase;

protected:
    std::expected<Submitted, AiError> submit(const MediaGenerationRequest& r, std::stop_token stop) override {
        const bool portrait = r.aspectRatio == "9:16";
        Json body{{"model", m_model.id}, {"prompt", r.prompt}, {"size", portrait ? "720x1280" : "1280x720"}};
        const int seconds = generatedSeconds(m_model.generation, r.duration);
        if (seconds > 0) {
            body["seconds"] = std::to_string(seconds);
        }
        if (!r.firstFrame.empty()) {
            auto url = imageDataUrl(r.firstFrame);
            if (!url) {
                return std::unexpected(url.error());
            }
            body["input_reference"] = Json{{"image_url", *url}};
        }
        auto j = postJson(endpointUrl(context()), body, m_provider.name, stop);
        if (!j) {
            return std::unexpected(j.error());
        }
        GenerationTicket ticket;
        ticket.id = jsonStringOr(*j, "id");
        if (!safePathId(ticket.id)) {
            return std::unexpected(malformedEvent("video job"));
        }
        return Submitted{std::nullopt, std::move(ticket)};
    }

    std::expected<GenerationStatus, AiError> status(const GenerationTicket& ticket, std::stop_token stop) override {
        if (!safePathId(ticket.id)) {
            return std::unexpected(invalid("invalid generation ticket"));
        }
        const std::string jobUrl = endpointUrl(context()) + "/" + ticket.id;
        auto j = getJson(m_http, context(), jobUrl, "video job", stop);
        if (!j) {
            return std::unexpected(j.error());
        }
        const std::string state = jsonStringOr(*j, "status");
        GenerationStatus s;
        s.progress = static_cast<float>(std::clamp(jsonNumberOr(*j, "progress", 0.0) / 100.0, 0.0, 1.0));
        if (state == "queued") {
            s.state = GenerationState::Queued;
        } else if (state == "in_progress") {
            s.state = GenerationState::Running;
        } else if (state == "completed") {
            s.state = GenerationState::Succeeded;
            s.outputUrls.push_back(jobUrl + "/content");
        } else if (state == "failed") {
            const std::string code = stringAt(*j, {"error", "code"});
            s = failedWith(m_provider.name + ": " + stringAt(*j, {"error", "message"}),
                           code.find("moderation") != std::string::npos || code.find("policy") != std::string::npos ? "content_policy" : code);
        } else {
            return std::unexpected(malformedEvent("video job status"));
        }
        return s;
    }

    std::expected<std::filesystem::path, AiError> download(const std::string& url, const std::filesystem::path& directory,
                                                           std::stop_token stop) override {
        if (!url.starts_with(endpointUrl(context()) + "/")) {
            return std::unexpected(invalid("generation returned an unapproved download host"));
        }
        // A custom or self-hosted server: its own scheme, host and port (plain http only on this computer, QA-04).
        if (m_provider.custom || m_provider.local) {
            return downloadFromProvider(m_http, url, providerHeaders(context()), directory, m_baseUrl, GeneratedMediaKind::Video, stop);
        }
        const auto base = parseHttpUrl(m_baseUrl);
        if (!base || base->scheme != "https") {
            return std::unexpected(invalid("generation returned an unapproved download host"));
        }
        return downloadGenerated(m_http, url, providerHeaders(context()), directory, {base->host}, GeneratedMediaKind::Video, stop);
    }
};

} // namespace

std::unique_ptr<IMediaGenerator> makeFalGenerator(ProviderContext context) { return std::make_unique<FalGenerator>(std::move(context)); }
std::unique_ptr<IMediaGenerator> makeReplicateGenerator(ProviderContext context) { return std::make_unique<ReplicateGenerator>(std::move(context)); }
std::unique_ptr<IMediaGenerator> makeXaiVideo(ProviderContext context) { return std::make_unique<XaiVideo>(std::move(context)); }
std::unique_ptr<IMediaGenerator> makeOpenAiVideos(ProviderContext context) { return std::make_unique<OpenAiVideos>(std::move(context)); }

} // namespace wabcut::ai::net
