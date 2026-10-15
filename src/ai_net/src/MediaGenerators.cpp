// Official Gemini Veo guide and Runway OpenAPI-generated SDK, checked 2026-10-11; see docs/ai.md.
#include "AdapterUtil.h"
#include <wabcut/ai_net/Adapters.h>
#include <wabcut/ai/Generation.h>
#include <wabcut/ai/JsonAccess.h>
#include <wabcut/ai/Redact.h>
#include <wabcut/ai/Redirects.h>
#include <wabcut/model/MediaLocator.h>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <random>
namespace wabcut::ai::net {
namespace {
/// The extension of a downloaded generated asset, from its first bytes (PNG, JPEG, WebP; ISO media "ftyp" for MP4/MOV)
/// checked against the Content-Type when that names a type (generic binary types defer to the signature). nullopt when
/// it is not the expected kind (an HTML error page, JSON, ...).
std::optional<std::string> generatedExtension(std::string contentType, std::string_view magic, bool image) {
    for (char& ch : contentType) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (const auto semicolon = contentType.find(';'); semicolon != std::string::npos) contentType.resize(semicolon);
    while (!contentType.empty() && contentType.back() == ' ') contentType.pop_back();
    const bool generic = contentType.empty() || contentType == "application/octet-stream" || contentType == "binary/octet-stream";
    const auto typed = [&](std::initializer_list<std::string_view> types) {
        return generic || std::ranges::any_of(types, [&](std::string_view t) { return contentType == t; });
    };
    if (image) {
        if (magic.starts_with(std::string_view("\x89PNG\r\n\x1a\n", 8)) && typed({"image/png"})) return ".png";
        if (magic.starts_with(std::string_view("\xff\xd8\xff", 3)) && typed({"image/jpeg", "image/jpg"})) return ".jpg";
        if (magic.size() >= 12 && magic.substr(0, 4) == "RIFF" && magic.substr(8, 4) == "WEBP" && typed({"image/webp"})) return ".webp";
        return std::nullopt;
    }
    if (magic.size() >= 8 && magic.substr(4, 4) == "ftyp" && typed({"video/mp4", "video/quicktime", "application/mp4"})) return ".mp4";
    return std::nullopt;
}

AiError invalid(std::string text) { return {AiErrorCode::InvalidRequest, std::move(text)}; }
bool safeId(std::string_view id, bool slash = false) {
    return !id.empty() && id.size() < 512 && id.find("..") == std::string_view::npos && id.find("//") == std::string_view::npos && std::all_of(id.begin(), id.end(), [slash](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || (slash && (c == '/' || c == '.'));
    });
}
std::expected<ImagePart, AiError> loadImage(const std::filesystem::path& path) {
    std::error_code ec; const auto size = std::filesystem::file_size(path, ec);
    if (ec || size == 0 || size > 3 * 1024 * 1024) return std::unexpected(invalid("conditioning image must be 1 byte to 3 MiB"));
    ImagePart image;
    const auto ext = pathToUtf8(path.extension());
    if (ext != ".png" && ext != ".jpg" && ext != ".jpeg") return std::unexpected(invalid("conditioning requires PNG or JPEG"));
    image.mediaType = ext == ".png" ? "image/png" : "image/jpeg";
    image.bytes.resize(static_cast<std::size_t>(size));
    std::ifstream file(path, std::ios::binary);
    file.read(reinterpret_cast<char*>(image.bytes.data()), static_cast<std::streamsize>(size));
    if (!file) return std::unexpected(invalid("cannot read conditioning image"));
    return image;
}
class Generator final : public IMediaGenerator {
public:
    explicit Generator(ProviderContext c, bool veo) : m_provider(c.provider), m_model(c.model),
        m_base(std::move(c.baseUrl)), m_key(std::move(c.key)), m_http(c.transport), m_veo(veo) {}
    const ModelSpec& spec() const override { return m_model; }
    std::expected<GenerationTicket, AiError> create(const MediaGenerationRequest& r, std::stop_token stop) override {
        if (auto valid = checkGenerationRequest(m_model, r); !valid) return std::unexpected(valid.error());
        if (!m_veo && (r.prompt.empty() || r.prompt.size() > 1000))
            return std::unexpected(invalid("Runway prompts must be 1 to 1000 UTF-8 bytes (conservative UTF-16 limit)"));
        if (r.count != 1) return std::unexpected(invalid("one output per run"));
        if (!r.inputClip.empty() || !r.lastFrame.empty() || !r.negativePrompt.empty())
            return std::unexpected(invalid("this adapter supports text and a first frame only"));
        std::optional<ImagePart> image;
        if (!r.firstFrame.empty()) { auto read = loadImage(r.firstFrame); if (!read) return std::unexpected(read.error()); image = std::move(*read); }
        Json body;
        std::string endpoint;
        m_image = r.kind == GeneratedMediaKind::Image;
        if (m_veo) {
            Json instance{{"prompt", r.prompt}};
            if (image) instance["image"] = {{"inlineData", {{"mimeType", image->mediaType}, {"data", base64Encode(image->bytes)}}}};
            body = {{"instances", Json::array({instance})}, {"parameters", {{"durationSeconds", std::to_string(generatedSeconds(m_model.generation, r.duration))},
                {"aspectRatio", r.aspectRatio}, {"resolution", r.resolution}}}};
            endpoint = "/v1beta/models/" + m_model.id + ":predictLongRunning";
        } else {
            const std::string ratio = m_image ? (r.aspectRatio == "1:1" ? "720:720" : r.aspectRatio == "9:16" ? "720:1280" : "1280:720")
                                             : (r.aspectRatio == "9:16" ? "720:1280" : "1280:720");
            body = {{"model", m_model.id}, {"promptText", r.prompt}, {"ratio", ratio}};
            if (!m_image) body["duration"] = generatedSeconds(m_model.generation, r.duration);
            if (image) {
                if (m_image) body["referenceImages"] = Json::array({{{"uri", dataUrl(*image)}}});
                else body["promptImage"] = dataUrl(*image);
            }
            endpoint = m_image ? "/v1/text_to_image" : image ? "/v1/image_to_video" : "/v1/text_to_video";
        }
        if (r.seed) {
            if (*r.seed < 0 || *r.seed > 4294967295LL) return std::unexpected(invalid("seed must fit an unsigned 32-bit integer"));
            if (m_veo) body["parameters"]["seed"] = *r.seed; else body["seed"] = *r.seed;
        }
        auto result = request("POST", endpoint, body, stop);
        if (!result) return std::unexpected(result.error());
        const std::string id = jsonStringOr(*result, m_veo ? "name" : "id");
        if (!safeId(id, m_veo) || (m_veo && !id.starts_with("models/")))
            return std::unexpected(malformedEvent("invalid generation job id"));
        return GenerationTicket{m_provider.id, m_model.id, id, {}, {}, {}};
    }
    std::expected<GenerationStatus, AiError> poll(const GenerationTicket& ticket, std::stop_token stop) override {
        if (!safeId(ticket.id, m_veo) || ticket.provider != m_provider.id || ticket.model != m_model.id)
            return std::unexpected(invalid("invalid generation ticket"));
        auto j = request("GET", m_veo ? "/v1beta/" + ticket.id : "/v1/tasks/" + ticket.id, {}, stop);
        if (!j) return std::unexpected(j.error());
        GenerationStatus status;
        if (m_veo) {
            if (const auto* error = jsonObject(*j, "error")) {
                status.state = GenerationState::Failed; status.failure = jsonStringOr(*error, "message");
                status.unbilled = true; return status;
            }
            if (!jsonBool(*j, "done").value_or(false)) { status.state = GenerationState::Running; return status; }
            if (const auto* response = jsonObject(*j, "response"))
                if (const auto* videos = jsonObject(*response, "generateVideoResponse"))
                    if (const auto* samples = jsonArray(*videos, "generatedSamples"))
                        for (const auto& sample : *samples) {
                            auto url = stringAt(sample, {"video", "uri"}); if (!url.empty()) status.outputUrls.push_back(url);
                        }
            status.state = status.outputUrls.empty() ? GenerationState::Failed : GenerationState::Succeeded;
            status.unbilled = status.outputUrls.empty();
            status.failure = "No video returned (possibly filtered by the provider)";
        } else {
            const auto state = jsonStringOr(*j, "status");
            if (state == "SUCCEEDED") status.state = GenerationState::Succeeded;
            else if (state == "FAILED") status.state = GenerationState::Failed;
            else if (state == "CANCELED" || state == "CANCELLED") status.state = GenerationState::Cancelled;
            else if (state == "RUNNING") status.state = GenerationState::Running;
            else if (state != "PENDING" && state != "THROTTLED") return std::unexpected(malformedEvent("unknown generation state"));
            status.failure = jsonStringOr(*j, "failure"); status.failureCode = jsonStringOr(*j, "failureCode");
            if (const auto* outputs = jsonArray(*j, "output")) for (const auto& url : *outputs)
                if (url.is_string()) status.outputUrls.push_back(url.get<std::string>());
        }
        if (status.outputUrls.size() > 1) return std::unexpected(malformedEvent("unexpected output count"));
        return status;
    }
    std::expected<std::vector<std::filesystem::path>, AiError> fetch(const GenerationStatus& status,
        const std::filesystem::path& directory, std::stop_token stop) override {
        if (status.state != GenerationState::Succeeded || status.outputUrls.size() != 1)
            return std::unexpected(malformedEvent("generation has no completed output"));
        const auto url = parseHttpUrl(status.outputUrls[0]);
        if (!url || url->scheme != "https" || !url->userinfo.empty() || (url->port && *url->port != 443) ||
            (m_veo ? url->host != "generativelanguage.googleapis.com" : url->host != "dnznrvs05pmza.cloudfront.net"))
            return std::unexpected(invalid("generation returned an unapproved download host"));
        HttpRequest http;
        http.method = "GET"; http.url = status.outputUrls[0]; http.responseMode = BodyMode::Raw;
        if (m_veo) http.headers = providerHeaders(context()); // never send Runway credentials to its CDN
        std::error_code ec; std::filesystem::create_directories(directory, ec);
        if (ec) return std::unexpected(invalid("cannot create generation directory"));
        const auto stem = directory / ("generated-" + std::to_string(std::random_device{}()));
        auto partial = stem; // appended to the path: stem.string() throws for names outside the code page
        partial += ".part";
        std::ofstream file(partial, std::ios::binary | std::ios::trunc);
        if (!file) return std::unexpected(invalid("cannot write generated asset"));
        std::uint64_t count = 0;
        std::string contentType;
        std::string magic; // the first bytes, to check what arrived (review wave 4, P2-7)
        HttpStreamHandlers handlers;
        handlers.onHead = [&](const HttpResponseHead& head) {
            if (auto type = findHeader(head.headers, "content-type")) contentType = std::string(*type);
            return true;
        };
        handlers.onData = [&](std::span<const std::byte> data) {
            count += data.size(); if (count > 512ULL * 1024 * 1024) return false;
            if (magic.size() < 16) magic.append(reinterpret_cast<const char*>(data.data()), std::min<std::size_t>(16 - magic.size(), data.size()));
            file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
            return static_cast<bool>(file);
        };
        // Redirects are followed on purpose and only on the download host itself (P1-4): a cross-host hop fails closed,
        // naming the host, and the provider key never travels with it. Google's download CDN host is not verified yet.
        auto response = sendFollowingRedirects(m_http, http, handlers, stop, RedirectAllowList::sameHost()); file.close();
        if (!response || !response->ok() || count == 0 || !file) {
            std::filesystem::remove(partial, ec);
            return std::unexpected(!response ? response.error() : !response->ok() ? httpError(*response) : invalid("empty or incomplete generated asset"));
        }
        // A 200 that is an error page or JSON is not an asset: the content type (when it names one) and the file's own
        // signature must agree with what was asked for, and they decide the extension (review wave 4, P2-7).
        const auto extension = generatedExtension(contentType, magic, m_image);
        if (!extension) {
            std::filesystem::remove(partial, ec);
            return std::unexpected(invalid(std::string("the provider returned something that is not ") + (m_image ? "a PNG, JPEG or WebP image" : "an MP4 video")));
        }
        auto path = stem;
        path += *extension;
        std::filesystem::rename(partial, path, ec);
        if (ec) { std::filesystem::remove(partial, ec); return std::unexpected(invalid("cannot finalize generated asset")); }
        return std::vector<std::filesystem::path>{path};
    }
    std::expected<void, AiError> cancel(const GenerationTicket& ticket, std::stop_token stop) override {
        if (m_veo) return IMediaGenerator::cancel(ticket, stop);
        if (!safeId(ticket.id)) return std::unexpected(invalid("invalid task id"));
        HttpRequest http; http.method = "DELETE"; http.url = m_base + "/v1/tasks/" + ticket.id;
        http.headers = providerHeaders(context());
        // Best effort, and it may run while the editor shuts down: a few seconds at most, no retries with back-off
        // (review wave 4, P2-8).
        http.timeout = std::chrono::seconds(5);
        http.idleTimeout = std::chrono::seconds(5);
        http.retrySafe = false;
        auto result = m_http.send(http, {}, stop);
        if (!result) return std::unexpected(result.error());
        if (!result->ok()) return std::unexpected(httpError(*result));
        return {};
    }
private:
    ProviderContext context() { return {m_provider, m_model, m_base, m_key.clone(), m_http}; }
    std::expected<Json, AiError> request(std::string method, std::string endpoint, const Json& body, std::stop_token stop) {
        auto http = jsonRequest(context(), m_base + endpoint, body, BodyMode::Buffered);
        http.method = method; http.retrySafe = method != "POST"; if (method == "GET") http.body.clear();
        auto response = m_http.send(http, {}, stop);
        if (!response) return std::unexpected(response.error());
        if (!response->ok()) return std::unexpected(httpError(*response));
        return parseJsonBody(response->body, "generation response");
    }
    ProviderSpec m_provider; ModelSpec m_model; std::string m_base; Secret m_key;
    IHttpTransport& m_http; bool m_veo = false, m_image = false;
};
}
std::unique_ptr<IMediaGenerator> makeVeoGenerator(ProviderContext c) { return std::make_unique<Generator>(std::move(c), true); }
std::unique_ptr<IMediaGenerator> makeRunwayGenerator(ProviderContext c) { return std::make_unique<Generator>(std::move(c), false); }
}
