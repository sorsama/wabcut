// Image generation adapters added in round 07 (docs/ai.md "Providers"), checked against the vendors' docs on 2026-11-03:
//   openai-images    POST {base}{endpoint = /images/generations}, bearer. OpenAI (gpt-image-2, gpt-image-1-mini):
//                    {"model", "prompt", "n", "size": "1024x1024" | "1536x1024" | "1024x1536", "quality": "medium",
//                    "output_format": "png"} -> {"data": [{"b64_json"}]}. xAI: {"model", "prompt", "n", "aspect_ratio",
//                    "response_format": "b64_json"}. Together: {"model", "prompt", "n", "width", "height", "seed",
//                    "response_format": "base64", "output_format": "png"}. Custom OpenAI-compatible servers: {"model",
//                    "prompt", "n", "size"}; a URL answer is downloaded from the server's own host only.
//                    developers.openai.com/api/reference/resources/images/methods/generate,
//                    docs.x.ai/developers/model-capabilities/images/generation, docs.together.ai/reference/post-images-generations
//   stability-image  POST {base}/v2beta/stable-image/generate/{ultra|core|sd3}, bearer, multipart (prompt, aspect_ratio,
//                    output_format=png, seed, negative_prompt, model for sd3), Accept: image/* -> the PNG; header
//                    finish-reason CONTENT_FILTERED = filtered. docs.stability.ai/api-reference
//   bfl-flux         POST {base}/v1/flux-3-image, x-key, {"prompt", "aspect_ratio", "resolution": "1k"} -> {"id",
//                    "polling_url"}; GET polling_url (api[.eu|.us].bfl.ai only, with the key) -> {"status": Pending |
//                    Ready | Content Moderated | ..., "result": {"sample": signed URL on *.bfl.ai, no key, 10 minutes}}.
//                    docs.bfl.ai/api_integration/integration_guidelines
//   ideogram         POST {base}/v2/image/generate/ideogram-3, Api-Key, {"prompt", "rendering_speed": turbo | default |
//                    quality, "num_images", "aspect_ratio": "16x9", "seed", "negative_prompt"} -> {"data": [{"url",
//                    "is_image_safe"}]} (URLs on ideogram.ai, expiring). developer.ideogram.ai/v2/api-reference
// Submissions are never retried: a network ambiguity must not bill an image twice.

#include "GeneratorBase.h"

#include <wabcut/ai_net/Adapters.h>

#include <algorithm>

namespace wabcut::ai::net {

namespace {

AiError invalid(std::string text) { return {AiErrorCode::InvalidRequest, std::move(text)}; }

GenerationStatus filtered(std::string message) {
    GenerationStatus s;
    s.state = GenerationState::Failed;
    s.failure = std::move(message);
    s.failureCode = "content_policy";
    return s;
}

/// Decodes the base64 images of an OpenAI-style answer ("b64_json"); URLs are collected for download.
std::expected<GenerationStatus, AiError> openAiStyleOutputs(const Json& j, std::string_view vendor) {
    GenerationStatus s;
    s.state = GenerationState::Succeeded;
    s.progress = 1.0f;
    const Json* data = jsonArray(j, "data");
    if (!data || data->empty()) {
        return std::unexpected(AiError(AiErrorCode::ProviderError, std::string(vendor) + " sent no image (possibly filtered)"));
    }
    for (const Json& item : *data) {
        if (!item.is_object()) {
            return std::unexpected(malformedEvent("image item"));
        }
        if (const std::string b64 = jsonStringOr(item, "b64_json"); !b64.empty()) {
            auto bytes = base64Decode(b64);
            if (!bytes || bytes->empty()) {
                return std::unexpected(malformedEvent("image data"));
            }
            s.inlineOutputs.push_back(std::move(*bytes));
            s.inlineTypes.emplace_back();
        } else if (const std::string url = jsonStringOr(item, "url"); !url.empty()) {
            s.outputUrls.push_back(url);
        } else {
            return std::unexpected(malformedEvent("image item without data"));
        }
    }
    s.usage.images = static_cast<std::int64_t>(s.inlineOutputs.size() + s.outputUrls.size());
    return s;
}

// --- OpenAI-compatible /images/generations ---------------------------------------------------------------------------

class OpenAiImages final : public GeneratorBase {
public:
    using GeneratorBase::GeneratorBase;

protected:
    std::expected<Submitted, AiError> submit(const MediaGenerationRequest& r, std::stop_token stop) override {
        if (!r.firstFrame.empty()) {
            return std::unexpected(invalid("this model makes images from a prompt alone"));
        }
        Json body{{"model", m_model.id}, {"prompt", r.prompt}, {"n", r.count}};
        const std::string& vendor = m_provider.id;
        if (vendor == "xai") {
            body["response_format"] = "b64_json";
            if (!r.aspectRatio.empty()) {
                body["aspect_ratio"] = r.aspectRatio;
            }
        } else if (vendor == "together") {
            const auto [w, h] = sizeForAspect(r.aspectRatio, 1024, 16);
            body["width"] = w;
            body["height"] = h;
            body["response_format"] = "base64";
            body["output_format"] = "png";
            if (r.seed) {
                body["seed"] = *r.seed;
            }
            if (!r.negativePrompt.empty()) {
                body["negative_prompt"] = r.negativePrompt;
            }
        } else {
            body["size"] = r.aspectRatio == "16:9" || r.aspectRatio == "3:2" || r.aspectRatio == "4:3" ? "1536x1024"
                           : r.aspectRatio == "9:16" || r.aspectRatio == "2:3" || r.aspectRatio == "3:4" ? "1024x1536"
                                                                                                         : "1024x1024";
            if (vendor == "openai") {
                body["quality"] = "medium"; // the registry prices the medium tier
                body["output_format"] = "png";
            }
        }
        const ProviderContext c = context();
        auto j = postJson(endpointUrl(c), body, m_provider.name, stop);
        if (!j) {
            return std::unexpected(j.error());
        }
        auto outputs = openAiStyleOutputs(*j, m_provider.name);
        if (!outputs) {
            return std::unexpected(outputs.error());
        }
        return Submitted{std::move(*outputs), {}};
    }

    std::expected<std::filesystem::path, AiError> download(const std::string& url, const std::filesystem::path& directory,
                                                           std::stop_token stop) override {
        // A custom or self-hosted server (LocalAI answers with URLs to itself): its own scheme, host and port, plain
        // http only on this computer (QA-04).
        if (m_provider.custom || m_provider.local) {
            return downloadFromProvider(m_http, url, providerHeaders(context()), directory, m_baseUrl, GeneratedMediaKind::Image, stop);
        }
        // A catalogue cloud provider: only its own host over HTTPS on 443, with its key.
        const auto base = parseHttpUrl(m_baseUrl);
        const auto target = parseHttpUrl(url);
        if (!base || !target || target->host != base->host || target->scheme != base->scheme) {
            return std::unexpected(invalid("generation returned an unapproved download host"));
        }
        if (base->scheme != "https") { // a local server: a plain download from itself
            return std::unexpected(invalid("only HTTPS image URLs are downloaded"));
        }
        return downloadGenerated(m_http, url, providerHeaders(context()), directory, {base->host}, GeneratedMediaKind::Image, stop);
    }
};

// --- Stability AI ------------------------------------------------------------------------------------------------------

class StabilityImages final : public GeneratorBase {
public:
    using GeneratorBase::GeneratorBase;

protected:
    std::expected<Submitted, AiError> submit(const MediaGenerationRequest& r, std::stop_token stop) override {
        if (!r.firstFrame.empty()) {
            return std::unexpected(invalid("this model makes images from a prompt alone"));
        }
        if (r.seed && (*r.seed < 0 || *r.seed > 4294967294LL)) {
            return std::unexpected(invalid("the seed must be 0 to 4294967294"));
        }
        std::vector<std::vector<std::uint8_t>> images;
        for (int i = 0; i < r.count; ++i) { // one image per request
            HttpRequest http;
            http.method = "POST";
            http.url = endpointUrl(context());
            http.headers = providerHeaders(context());
            http.headers.push_back({"accept", "image/*"});
            http.multipart.push_back({"prompt", r.prompt, {}, {}, {}});
            if (!r.aspectRatio.empty()) {
                http.multipart.push_back({"aspect_ratio", r.aspectRatio, {}, {}, {}});
            }
            http.multipart.push_back({"output_format", "png", {}, {}, {}});
            if (r.seed) {
                http.multipart.push_back({"seed", std::to_string(*r.seed + i), {}, {}, {}});
            }
            if (!r.negativePrompt.empty()) {
                http.multipart.push_back({"negative_prompt", r.negativePrompt, {}, {}, {}});
            }
            if (m_model.endpoint.ends_with("/sd3")) {
                http.multipart.push_back({"model", m_model.id, {}, {}, {}});
            }
            auto response = m_http.send(paidSubmit(std::move(http)), {}, stop);
            if (!response) {
                return std::unexpected(response.error());
            }
            if (!response->ok()) {
                return std::unexpected(httpError(*response));
            }
            if (const auto reason = findHeader(response->headers, "finish-reason"); reason && *reason == "CONTENT_FILTERED") {
                return Submitted{filtered("Stability AI filtered the image"), {}};
            }
            images.emplace_back(response->body.begin(), response->body.end());
        }
        GenerationStatus done = doneWith(std::move(images), std::vector<std::string>(static_cast<std::size_t>(r.count), "image/png"));
        done.usage.images = r.count;
        return Submitted{std::move(done), {}};
    }

    std::expected<std::filesystem::path, AiError> download(const std::string&, const std::filesystem::path&, std::stop_token) override {
        return std::unexpected(invalid("Stability AI answers with the image itself"));
    }
};

// --- Black Forest Labs -------------------------------------------------------------------------------------------------

class BflImages final : public GeneratorBase {
public:
    using GeneratorBase::GeneratorBase;

protected:
    std::expected<Submitted, AiError> submit(const MediaGenerationRequest& r, std::stop_token stop) override {
        if (r.count != 1) {
            return std::unexpected(invalid("one image per run"));
        }
        Json body{{"prompt", r.prompt}, {"resolution", "1k"}};
        if (!r.aspectRatio.empty()) {
            body["aspect_ratio"] = r.aspectRatio;
        }
        if (!r.firstFrame.empty()) {
            auto image = loadConditioningImage(r.firstFrame);
            if (!image) {
                return std::unexpected(image.error());
            }
            body["images"] = Json::array({base64Encode(image->bytes)});
        }
        auto j = postJson(endpointUrl(context()), body, "Black Forest Labs", stop);
        if (!j) {
            return std::unexpected(j.error());
        }
        const std::string id = jsonStringOr(*j, "id");
        const std::string polling = jsonStringOr(*j, "polling_url");
        if (!safePathId(id) || !apiHost(polling)) {
            return std::unexpected(malformedEvent("Black Forest Labs job"));
        }
        GenerationTicket ticket;
        ticket.id = id;
        ticket.pollUrl = polling;
        return Submitted{std::nullopt, std::move(ticket)};
    }

    std::expected<GenerationStatus, AiError> status(const GenerationTicket& ticket, std::stop_token stop) override {
        if (!apiHost(ticket.pollUrl) || !safePathId(ticket.id)) {
            return std::unexpected(invalid("invalid generation ticket"));
        }
        auto j = getJson(m_http, context(), ticket.pollUrl, "Black Forest Labs result", stop);
        if (!j) {
            return std::unexpected(j.error());
        }
        GenerationStatus s;
        const std::string state = jsonStringOr(*j, "status");
        if (state == "Ready") {
            const std::string sample = stringAt(*j, {"result", "sample"});
            if (sample.empty()) {
                return std::unexpected(malformedEvent("Black Forest Labs result"));
            }
            s.state = GenerationState::Succeeded;
            s.progress = 1.0f;
            s.outputUrls.push_back(sample);
            s.usage.images = 1;
        } else if (state == "Pending" || state == "Reasoning" || state == "Generating") {
            s.state = GenerationState::Running;
            s.progress = static_cast<float>(std::clamp(jsonNumberOr(*j, "progress", 0.0), 0.0, 1.0));
        } else if (state == "Request Moderated" || state == "Content Moderated") {
            s = filtered("Black Forest Labs moderated the " + std::string(state == "Request Moderated" ? "request" : "image"));
        } else if (state == "Error" || state == "Task not found" || state == "No Card Generated") {
            s.state = GenerationState::Failed;
            s.failure = "Black Forest Labs: " + state;
        } else {
            return std::unexpected(malformedEvent("Black Forest Labs status"));
        }
        return s;
    }

    std::expected<std::filesystem::path, AiError> download(const std::string& url, const std::filesystem::path& directory,
                                                           std::stop_token stop) override {
        // Signed delivery URLs (delivery.<region>.bfl.ai): no key goes with them.
        return downloadGenerated(m_http, url, {}, directory, {"*.bfl.ai"}, GeneratedMediaKind::Image, stop);
    }

private:
    /// The API hosts a polling URL may point at (the key goes there).
    static bool apiHost(std::string_view url) {
        const auto parts = parseHttpUrl(url);
        return parts && parts->scheme == "https" && parts->userinfo.empty() && (!parts->port || *parts->port == 443) &&
               hostMatches(parts->host, {"api.bfl.ai", "api.eu.bfl.ai", "api.us.bfl.ai"});
    }
};

// --- Ideogram ----------------------------------------------------------------------------------------------------------

class IdeogramImages final : public GeneratorBase {
public:
    using GeneratorBase::GeneratorBase;

protected:
    std::expected<Submitted, AiError> submit(const MediaGenerationRequest& r, std::stop_token stop) override {
        if (!r.firstFrame.empty()) {
            return std::unexpected(invalid("this model makes images from a prompt alone"));
        }
        std::string speed = "default";
        if (m_model.id.ends_with("-turbo")) {
            speed = "turbo";
        } else if (m_model.id.ends_with("-quality")) {
            speed = "quality";
        }
        std::string ratio = r.aspectRatio.empty() ? std::string("1:1") : r.aspectRatio;
        std::replace(ratio.begin(), ratio.end(), ':', 'x');
        Json body{{"prompt", r.prompt}, {"rendering_speed", speed}, {"num_images", r.count}, {"aspect_ratio", ratio}};
        if (r.seed) {
            if (*r.seed < 0 || *r.seed > 2147483647LL) {
                return std::unexpected(invalid("the seed must be 0 to 2147483647"));
            }
            body["seed"] = *r.seed;
        }
        if (!r.negativePrompt.empty()) {
            body["negative_prompt"] = r.negativePrompt;
        }
        auto j = postJson(endpointUrl(context()), body, "Ideogram", stop);
        if (!j) {
            return std::unexpected(j.error());
        }
        GenerationStatus s;
        s.state = GenerationState::Succeeded;
        s.progress = 1.0f;
        if (const Json* data = jsonArray(*j, "data")) {
            for (const Json& item : *data) {
                const std::string url = item.is_object() ? jsonStringOr(item, "url") : std::string();
                if (!url.empty()) {
                    s.outputUrls.push_back(url);
                }
            }
        }
        if (s.outputUrls.empty()) {
            return Submitted{filtered("Ideogram did not return a safe image"), {}};
        }
        s.usage.images = static_cast<std::int64_t>(s.outputUrls.size());
        return Submitted{std::move(s), {}};
    }

    std::expected<std::filesystem::path, AiError> download(const std::string& url, const std::filesystem::path& directory,
                                                           std::stop_token stop) override {
        return downloadGenerated(m_http, url, {}, directory, {"ideogram.ai"}, GeneratedMediaKind::Image, stop);
    }
};

} // namespace

std::unique_ptr<IMediaGenerator> makeOpenAiImages(ProviderContext context) { return std::make_unique<OpenAiImages>(std::move(context)); }
std::unique_ptr<IMediaGenerator> makeStabilityImages(ProviderContext context) { return std::make_unique<StabilityImages>(std::move(context)); }
std::unique_ptr<IMediaGenerator> makeBflImages(ProviderContext context) { return std::make_unique<BflImages>(std::move(context)); }
std::unique_ptr<IMediaGenerator> makeIdeogramImages(ProviderContext context) { return std::make_unique<IdeogramImages>(std::move(context)); }

} // namespace wabcut::ai::net
