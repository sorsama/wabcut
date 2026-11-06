// r07 generation contracts on fake transports: OpenAI-compatible images (base64), Stability AI (image bytes, filtered),
// Black Forest Labs (polling URL on its API hosts, signed delivery without the key), Ideogram (expiring URLs), fal.ai's
// queue (image, video, sound effects, cancel), Replicate predictions (download with the token), xAI video and the
// OpenAI-compatible /videos API. Request shape, auth header, host allow-lists, error mapping, cancel, spend.

#include "FakeTransport.h"

#include "AdapterUtil.h" // src/ai_net/src: the adapters' shared helpers

#include <wabcut/ai/Generation.h>
#include <wabcut/ai/Provider.h>
#include <wabcut/ai/Transport.h>
#include <wabcut/ai_net/Adapters.h>

#include <catch2/catch_test_macros.hpp>

using namespace wabcut;
using namespace wabcut::ai;
using namespace wabcut::ai::test;

namespace {

const std::string kPng("\x89PNG\r\n\x1a\n\0\0\0\rIHDR", 16);
const std::string kMp4("\x00\x00\x00\x18" "ftypisom" "\x00\x00\x02\x00" "isomiso2", 24);
const std::string kMp3("ID3\x03\x00\x00\x00\x00\x00\x00" "frames", 16);

ScriptedResponse asset(std::string body, std::string type) {
    ScriptedResponse r;
    r.headers = {{"content-type", std::move(type)}};
    r.body = std::move(body);
    return r;
}

std::string b64(std::string_view bytes) {
    return net::base64Encode(std::span(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
}

struct Fixture {
    ModelRegistry models = ModelRegistry::builtin();
    ProviderRegistry providers{models};
    MemorySecretStore secrets;
    KeyRing keys{secrets};
    ConsentLedger consent;
    SpendGuard spend;
    RequestLog log;
    FakeTransport fake;
    AiPlatform ai{providers, keys, consent, spend, log, fake};
    std::filesystem::path directory = uniqueTempPath("wabcut-r07-generation");

    Fixture() {
        net::registerCloudAdapters(providers);
        for (const char* p : {"openai", "stability", "bfl", "ideogram", "fal", "replicate"}) {
            REQUIRE(models.provider(p) != nullptr);
            (void)keys.save(*models.provider(p), Secret(std::string("fixture-only-") + p + "-key"));
        }
        ai.setPolling([](auto, auto) { return true; }, std::chrono::milliseconds(0), std::chrono::seconds(5));
    }
    ~Fixture() {
        std::error_code ec;
        std::filesystem::remove_all(directory, ec);
    }
    static MediaGenerationRequest image(std::string aspect = "16:9") {
        MediaGenerationRequest r;
        r.kind = GeneratedMediaKind::Image;
        r.prompt = "a lighthouse at dusk";
        r.aspectRatio = std::move(aspect);
        r.resolution = "720p";
        return r;
    }
    static MediaGenerationRequest video(int seconds = 5) {
        MediaGenerationRequest r;
        r.kind = GeneratedMediaKind::Video;
        r.prompt = "a drone shot over a harbour";
        r.duration = Ticks::fromSeconds(seconds);
        r.resolution = "720p";
        return r;
    }
    static CallOptions approved(MicroUsd cap = 5'000'000) { return CallOptions{true, cap}; }
};

/// The file's bytes.
std::string contents(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

} // namespace

TEST_CASE("OpenAI images: medium quality / size from the aspect / base64 PNG / priced per image", "[ai][net][generation][r07]") {
    Fixture f;
    f.fake.pushJson(200, Json{{"created", 1}, {"data", Json::array({Json{{"b64_json", b64(kPng)}}})}}.dump());
    auto result = f.ai.generate("gpt-image-2", Fixture::image(), f.directory, {}, {}, Fixture::approved());
    REQUIRE(result);
    REQUIRE(result->files.size() == 1);
    CHECK(result->files[0].extension() == ".png");
    CHECK(contents(result->files[0]) == kPng);
    CHECK(result->cost == 53000);
    const auto requests = f.fake.requests();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].url == "https://api.openai.com/v1/images/generations");
    CHECK_FALSE(requests[0].retrySafe);
    const Json body = Json::parse(requests[0].body);
    CHECK(body["model"] == "gpt-image-2");
    CHECK(body["size"] == "1536x1024");
    CHECK(body["quality"] == "medium");
    CHECK(body["n"] == 1);
    CHECK_FALSE(body.contains("response_format"));

    // An answer without images (filtered) is an error; a "PNG" that is HTML is refused.
    f.fake.pushJson(200, R"({"data":[]})");
    CHECK_FALSE(f.ai.generate("gpt-image-2", Fixture::image(), f.directory, {}, {}, Fixture::approved()));
    f.fake.pushJson(200, Json{{"data", Json::array({Json{{"b64_json", b64("<html>no</html>")}}})}}.dump());
    auto html = f.ai.generate("gpt-image-2", Fixture::image(), f.directory, {}, {}, Fixture::approved());
    REQUIRE_FALSE(html);
    CHECK(html.error().code == AiErrorCode::InvalidRequest);
}

TEST_CASE("Stability AI: multipart form / image bytes back / filtered images fail", "[ai][net][generation][r07]") {
    Fixture f;
    f.fake.push(asset(kPng, "image/png"));
    MediaGenerationRequest request = Fixture::image("21:9");
    request.seed = 42;
    request.negativePrompt = "people";
    auto result = f.ai.generate("stable-image-core", request, f.directory, {}, {}, Fixture::approved());
    REQUIRE(result);
    CHECK(result->cost == 30000);
    const HttpRequest sent = f.fake.requests().at(0);
    CHECK(sent.url == "https://api.stability.ai/v2beta/stable-image/generate/core");
    CHECK(findHeader(sent.headers, "accept") == "image/*");
    CHECK(findHeader(sent.headers, "Authorization") == "Bearer fixture-only-stability-key");
    const auto field = [&](std::string_view name) {
        for (const auto& m : sent.multipart) {
            if (m.name == name) {
                return m.value;
            }
        }
        return std::string("<missing>");
    };
    CHECK(field("prompt") == "a lighthouse at dusk");
    CHECK(field("aspect_ratio") == "21:9");
    CHECK(field("seed") == "42");
    CHECK(field("negative_prompt") == "people");
    CHECK(field("model") == "<missing>");

    ScriptedResponse blurred = asset(kPng, "image/png");
    blurred.headers.push_back({"finish-reason", "CONTENT_FILTERED"});
    f.fake.push(blurred);
    auto filtered = f.ai.generate("sd3.5-large", Fixture::image(), f.directory, {}, {}, Fixture::approved());
    REQUIRE_FALSE(filtered);
    CHECK(filtered.error().code == AiErrorCode::InvalidRequest); // refused content: retrying will not help
    CHECK(f.fake.requests().at(1).multipart.back().name == "model");

    f.fake.pushJson(402, R"({"errors":["insufficient credits"],"name":"payment_required"})");
    auto broke = f.ai.generate("stable-image-ultra", Fixture::image(), f.directory, {}, {}, Fixture::approved());
    REQUIRE_FALSE(broke);
    CHECK(broke.error().code == AiErrorCode::QuotaExceeded);
}

TEST_CASE("Black Forest Labs: x-key / polling URL on the API hosts only / signed delivery without the key", "[ai][net][generation][r07]") {
    Fixture f;
    f.fake.pushJson(200, R"({"id":"task-1","polling_url":"https://api.eu.bfl.ai/v1/get_result?id=task-1","cost":4.8})");
    f.fake.pushJson(200, R"({"id":"task-1","status":"Pending","progress":0.4})");
    f.fake.pushJson(200, R"({"id":"task-1","status":"Ready","result":{"sample":"https://delivery-eu1.bfl.ai/results/x/sample.png?se=1&sig=s"}})");
    f.fake.push(asset(kPng, "image/png"));
    auto result = f.ai.generate("flux-3-image", Fixture::image(), f.directory, {}, {}, Fixture::approved());
    REQUIRE(result);
    CHECK(result->cost == 48000);
    const auto requests = f.fake.requests();
    REQUIRE(requests.size() == 4);
    CHECK(requests[0].url == "https://api.bfl.ai/v1/flux-3-image");
    CHECK(findHeader(requests[0].headers, "x-key") == "fixture-only-bfl-key");
    CHECK(Json::parse(requests[0].body)["aspect_ratio"] == "16:9");
    CHECK(requests[1].url == "https://api.eu.bfl.ai/v1/get_result?id=task-1");
    CHECK(findHeader(requests[1].headers, "x-key").has_value());
    CHECK_FALSE(findHeader(requests[3].headers, "x-key")); // the signed URL needs no key, and gets none

    SECTION("a polling URL elsewhere is refused before the key goes there") {
        f.fake.pushJson(200, R"({"id":"task-2","polling_url":"https://evil.example/v1/get_result?id=task-2"})");
        CHECK_FALSE(f.ai.generate("flux-3-image", Fixture::image(), f.directory, {}, {}, Fixture::approved()));
        CHECK(f.fake.requests().size() == 5);
    }
    SECTION("moderated content fails as refused content") {
        f.fake.pushJson(200, R"({"id":"task-3","polling_url":"https://api.bfl.ai/v1/get_result?id=task-3"})");
        f.fake.pushJson(200, R"({"id":"task-3","status":"Content Moderated"})");
        auto moderated = f.ai.generate("flux-3-image", Fixture::image(), f.directory, {}, {}, Fixture::approved());
        REQUIRE_FALSE(moderated);
        CHECK(moderated.error().code == AiErrorCode::InvalidRequest);
    }
}

TEST_CASE("Ideogram: Api-Key / rendering speed from the model / URLs on ideogram.ai only", "[ai][net][generation][r07]") {
    Fixture f;
    f.fake.pushJson(200, R"({"generation_id":"g1","data":[{"url":"https://ideogram.ai/api/images/ephemeral/abc.png?exp=1&sig=2","is_image_safe":true}]})");
    f.fake.push(asset(kPng, "image/png"));
    auto result = f.ai.generate("ideogram-3-turbo", Fixture::image("9:16"), f.directory, {}, {}, Fixture::approved());
    REQUIRE(result);
    CHECK(result->cost == 30000);
    const auto requests = f.fake.requests();
    CHECK(requests[0].url == "https://api.ideogram.ai/v2/image/generate/ideogram-3");
    CHECK(findHeader(requests[0].headers, "Api-Key") == "fixture-only-ideogram-key");
    const Json body = Json::parse(requests[0].body);
    CHECK(body["rendering_speed"] == "turbo");
    CHECK(body["aspect_ratio"] == "9x16");
    CHECK_FALSE(findHeader(requests[1].headers, "Api-Key"));

    f.fake.pushJson(200, R"({"data":[{"url":"https://cdn.evil.example/abc.png"}]})");
    auto evil = f.ai.generate("ideogram-3", Fixture::image(), f.directory, {}, {}, Fixture::approved());
    REQUIRE_FALSE(evil);
    CHECK(f.fake.requests().size() == 3); // nothing was fetched from the other host
    f.fake.pushJson(200, R"({"data":[{"url":"","is_image_safe":false}]})");
    auto unsafe = f.ai.generate("ideogram-3-quality", Fixture::image(), f.directory, {}, {}, Fixture::approved());
    REQUIRE_FALSE(unsafe);
    CHECK(unsafe.error().code == AiErrorCode::InvalidRequest);
}

TEST_CASE("fal.ai queue: Key auth / queue URLs / model inputs / public CDN download / cancel", "[ai][net][generation][r07]") {
    Fixture f;
    const std::string queued = R"({"request_id":"req-1","status_url":"https://queue.fal.run/fal-ai/kling-video/requests/req-1/status",
        "response_url":"https://queue.fal.run/fal-ai/kling-video/requests/req-1","cancel_url":"https://queue.fal.run/fal-ai/kling-video/requests/req-1/cancel"})";
    f.fake.pushJson(200, queued);
    f.fake.pushJson(200, R"({"status":"IN_QUEUE","queue_position":2})");
    f.fake.pushJson(200, R"({"status":"COMPLETED"})");
    f.fake.pushJson(200, R"({"video":{"url":"https://v3.fal.media/files/out.mp4","content_type":"video/mp4"}})");
    f.fake.push(asset(kMp4, "video/mp4"));
    MediaGenerationRequest request = Fixture::video(5);
    request.aspectRatio = "9:16";
    request.audio = true;
    auto result = f.ai.generate("fal-ai/kling-video/v3/pro/text-to-video", request, f.directory, {}, {}, Fixture::approved());
    REQUIRE(result);
    CHECK(result->files.at(0).extension() == ".mp4");
    CHECK(result->cost == 840000); // 5 s with sound at $0.168
    auto requests = f.fake.requests();
    REQUIRE(requests.size() == 5);
    CHECK(requests[0].url == "https://queue.fal.run/fal-ai/kling-video/v3/pro/text-to-video");
    CHECK(findHeader(requests[0].headers, "Authorization") == "Key fixture-only-fal-key");
    const Json input = Json::parse(requests[0].body);
    CHECK(input["duration"] == "5");
    CHECK(input["aspect_ratio"] == "9:16");
    CHECK(input["generate_audio"] == true);
    CHECK(requests[1].url == "https://queue.fal.run/fal-ai/kling-video/requests/req-1/status");
    CHECK_FALSE(findHeader(requests[4].headers, "Authorization"));

    // Sound effects: ElevenLabs on fal takes "text" and "duration_seconds".
    f.fake.pushJson(200, queued);
    f.fake.pushJson(200, R"({"status":"COMPLETED"})");
    f.fake.pushJson(200, R"({"audio":{"url":"https://fal.media/files/boom.mp3"}})");
    f.fake.push(asset(kMp3, "audio/mpeg"));
    MediaGenerationRequest sound;
    sound.kind = GeneratedMediaKind::SoundEffect;
    sound.prompt = "a heavy door slams";
    sound.aspectRatio.clear();
    sound.resolution.clear();
    sound.duration = Ticks::fromSeconds(3);
    auto boom = f.ai.generate("fal-ai/elevenlabs/sound-effects/v2", sound, f.directory, {}, {}, Fixture::approved());
    REQUIRE(boom);
    CHECK(boom->files.at(0).extension() == ".mp3");
    CHECK(boom->cost == 6000); // 3 s at $0.12 a minute
    requests = f.fake.requests();
    const Json soundInput = Json::parse(requests[5].body);
    CHECK(soundInput["text"] == "a heavy door slams");
    CHECK(soundInput["duration_seconds"] == 3.0);

    SECTION("queue URLs on another host are refused") {
        f.fake.pushJson(200, R"({"request_id":"r","status_url":"https://evil.example/s","response_url":"https://queue.fal.run/x/requests/r"})");
        CHECK_FALSE(f.ai.generate("fal-ai/flux/dev", Fixture::image(), f.directory, {}, {}, Fixture::approved()));
        CHECK(f.fake.requests().size() == 10);
    }
    SECTION("an output URL off fal's CDN is refused") {
        f.fake.pushJson(200, queued);
        f.fake.pushJson(200, R"({"status":"COMPLETED"})");
        f.fake.pushJson(200, R"({"images":[{"url":"https://fal.media.evil.example/x.png"}]})");
        CHECK_FALSE(f.ai.generate("fal-ai/flux/dev", Fixture::image(), f.directory, {}, {}, Fixture::approved()));
        CHECK(f.fake.requests().size() == 12);
    }
    SECTION("cancelling a submitted job PUTs its cancel URL") {
        f.fake.pushJson(200, queued);
        f.fake.pushJson(202, R"({"status":"CANCELLATION_REQUESTED"})");
        std::stop_source stop;
        GenerationEvents events;
        events.onSubmitted = [&](const GenerationTicket&) { stop.request_stop(); };
        auto cancelled = f.ai.generate("fal-ai/flux/dev", Fixture::image(), f.directory, events, stop.get_token(), Fixture::approved());
        REQUIRE_FALSE(cancelled);
        CHECK(cancelled.error().code == AiErrorCode::Cancelled);
        const auto all = f.fake.requests();
        CHECK(all.back().method == "PUT");
        CHECK(all.back().url == "https://queue.fal.run/fal-ai/kling-video/requests/req-1/cancel");
        CHECK(all.back().timeout <= std::chrono::seconds(5));
    }
}

TEST_CASE("Replicate: predictions API / output array / delivery download with the token / cancel", "[ai][net][generation][r07]") {
    Fixture f;
    f.fake.pushJson(201, R"({"id":"p123","status":"starting","urls":{"get":"https://api.replicate.com/v1/predictions/p123"}})");
    f.fake.pushJson(200, R"({"id":"p123","status":"processing"})");
    f.fake.pushJson(200, R"({"id":"p123","status":"succeeded","output":["https://replicate.delivery/xezq/out-0.png","https://replicate.delivery/xezq/out-1.png"]})");
    f.fake.push(asset(kPng, "image/png"));
    f.fake.push(asset(kPng, "image/png"));
    MediaGenerationRequest request = Fixture::image("1:1");
    request.count = 2;
    auto result = f.ai.generate("black-forest-labs/flux-schnell", request, f.directory, {}, {}, Fixture::approved());
    REQUIRE(result);
    CHECK(result->files.size() == 2);
    CHECK(result->cost == 6000);
    const auto requests = f.fake.requests();
    REQUIRE(requests.size() == 5);
    CHECK(requests[0].url == "https://api.replicate.com/v1/models/black-forest-labs/flux-schnell/predictions");
    const Json body = Json::parse(requests[0].body);
    CHECK(body["input"]["num_outputs"] == 2);
    CHECK(body["input"]["aspect_ratio"] == "1:1");
    CHECK(requests[1].url == "https://api.replicate.com/v1/predictions/p123");
    CHECK(findHeader(requests[3].headers, "Authorization") == "Bearer fixture-only-replicate-key");

    f.fake.pushJson(201, R"({"id":"p124","status":"starting"})");
    f.fake.pushJson(200, R"({"id":"p124","status":"failed","error":"NSFW content detected"})");
    auto failed = f.ai.generate("black-forest-labs/flux-1.1-pro", Fixture::image(), f.directory, {}, {}, Fixture::approved());
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == AiErrorCode::InvalidRequest);

    f.fake.pushJson(201, R"({"id":"p125","status":"starting"})");
    f.fake.pushJson(200, R"({"id":"p125","status":"succeeded","output":"https://evil.example/out.png"})");
    CHECK_FALSE(f.ai.generate("black-forest-labs/flux-1.1-pro", Fixture::image(), f.directory, {}, {}, Fixture::approved()));
    CHECK(f.fake.requests().size() == 9);
}

TEST_CASE("xAI video and OpenAI-compatible videos: job shape / poll / download rules", "[ai][net][generation][r07]") {
    ProviderSpec xai;
    xai.id = "xai-fixture";
    xai.name = "xAI";
    xai.baseUrl = "https://api.x.ai/v1";
    xai.auth = AuthScheme::Bearer;
    ModelSpec grok;
    grok.id = "grok-imagine-video-1.5";
    grok.provider = xai.id;
    grok.name = "Grok Imagine Video";
    grok.api = "xai-video";
    grok.endpoint = "/videos/generations";
    grok.caps.capabilities = {Capability::GenerateMedia};
    grok.generation.video = true;
    grok.generation.audio = true;
    grok.generation.durations = {5, 8, 10, 15};
    FakeTransport fake;
    const std::filesystem::path directory = uniqueTempPath("wabcut-r07-video");
    auto generator = net::makeXaiVideo(ProviderContext{xai, grok, xai.baseUrl, Secret("fixture-only-xai-key"), fake});
    fake.pushJson(200, R"({"request_id":"vid-1"})");
    MediaGenerationRequest request = Fixture::video(6);
    request.aspectRatio = "16:9";
    auto ticket = generator->create(request, {});
    REQUIRE(ticket);
    const Json body = Json::parse(fake.requests().at(0).body);
    CHECK(fake.requests().at(0).url == "https://api.x.ai/v1/videos/generations");
    CHECK(body["duration"] == 8);
    CHECK(body["resolution"] == "720p");
    CHECK(body["generate_audio"] == false);
    fake.pushJson(202, R"({"status":"pending"})");
    auto pending = generator->poll(*ticket, {});
    REQUIRE(pending);
    CHECK(pending->state == GenerationState::Running);
    CHECK(fake.requests().at(1).url == "https://api.x.ai/v1/videos/vid-1");
    fake.pushJson(200, R"({"status":"done","video":{"url":"https://vidgen.x.ai/out/vid-1.mp4","duration":8}})");
    auto done = generator->poll(*ticket, {});
    REQUIRE(done);
    REQUIRE(done->state == GenerationState::Succeeded);
    fake.push(asset(kMp4, "video/mp4"));
    auto files = generator->fetch(*done, directory, {});
    REQUIRE(files);
    CHECK_FALSE(findHeader(fake.requests().at(3).headers, "Authorization"));
    fake.pushJson(200, R"({"status":"failed","error":{"code":"invalid_argument","message":"prompt refused"}})");
    auto refused = generator->poll(*ticket, {});
    REQUIRE(refused);
    CHECK(refused->state == GenerationState::Failed);
    CHECK(refused->failureCode == "invalid");

    // An OpenAI-compatible server's /videos: the content comes from the server itself, with its key.
    ProviderSpec custom = xai;
    custom.id = "custom-video";
    custom.baseUrl = "https://video.example.com/v1";
    ModelSpec sora = grok;
    sora.id = "video-model";
    sora.provider = custom.id;
    sora.api = "openai-videos";
    sora.endpoint = "/videos";
    sora.generation.durations = {4, 8, 12};
    auto videos = net::makeOpenAiVideos(ProviderContext{custom, sora, custom.baseUrl, Secret("fixture-only-custom-key"), fake});
    fake.pushJson(200, R"({"id":"video_abc","status":"queued"})");
    auto job = videos->create(Fixture::video(4), {});
    REQUIRE(job);
    const Json videoBody = Json::parse(fake.requests().back().body);
    CHECK(videoBody["seconds"] == "4");
    CHECK(videoBody["size"] == "1280x720");
    fake.pushJson(200, R"({"id":"video_abc","status":"completed","progress":100})");
    auto completed = videos->poll(*job, {});
    REQUIRE(completed);
    REQUIRE(completed->outputUrls.size() == 1);
    CHECK(completed->outputUrls[0] == "https://video.example.com/v1/videos/video_abc/content");
    fake.push(asset(kMp4, "application/octet-stream"));
    REQUIRE(videos->fetch(*completed, directory, {}));
    CHECK(findHeader(fake.requests().back().headers, "Authorization") == "Bearer fixture-only-custom-key");
    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
}

TEST_CASE("Generated-media helpers: host patterns and file signatures", "[ai][net][generation][r07]") {
    CHECK(net::hostMatches("v3.fal.media", {"*.fal.media"}));
    CHECK_FALSE(net::hostMatches("fal.media", {"*.fal.media"}));
    CHECK_FALSE(net::hostMatches("evilfal.media", {"*.fal.media"}));
    CHECK_FALSE(net::hostMatches("fal.media.evil.example", {"*.fal.media"}));
    CHECK(net::hostMatches("ideogram.ai", {"ideogram.ai"}));
    CHECK(net::generatedFileExtension("audio/mpeg", kMp3, GeneratedMediaKind::SoundEffect) == ".mp3");
    CHECK(net::generatedFileExtension("", std::string_view("RIFF\x10\0\0\0WAVEfmt ", 16), GeneratedMediaKind::SoundEffect) == ".wav");
    CHECK_FALSE(net::generatedFileExtension("text/html", kPng, GeneratedMediaKind::Image));
    CHECK_FALSE(net::generatedFileExtension("video/mp4", kPng, GeneratedMediaKind::Video));
    CHECK(net::sizeForAspect("16:9", 1024) == std::pair{1024, 576});
    CHECK(net::sizeForAspect("9:16", 1024) == std::pair{576, 1024});
}

TEST_CASE("Custom and self-hosted servers: generated media downloads come from their own origin only", "[ai][net][generation][r07][custom]") {
    // QA-04: a server on this computer (plain http, its own port) or a gateway on a custom HTTPS port answers with URLs
    // to itself; those download, anything off that origin does not, and catalogue cloud providers keep their rules.
    FakeTransport fake;
    const std::filesystem::path directory = uniqueTempPath("wabcut-r07-own-origin");
    ProviderSpec office;
    office.id = "custom-office";
    office.name = "Office server";
    office.baseUrl = "http://127.0.0.1:8080/v1";
    office.auth = AuthScheme::None;
    office.keyRequired = false;
    office.custom = true;
    office.local = true;
    ModelSpec video;
    video.id = "office-video";
    video.provider = office.id;
    video.name = "office-video";
    video.api = "openai-videos";
    video.endpoint = "/videos";
    video.caps.capabilities = {Capability::GenerateMedia};
    video.generation.video = true;
    video.generation.durations = {4, 8, 12};

    const auto runVideo = [&](const ProviderSpec& provider, std::string key) {
        auto videos = net::makeOpenAiVideos(ProviderContext{provider, video, provider.baseUrl, Secret(std::move(key)), fake});
        fake.pushJson(200, R"({"id":"video_1","status":"queued"})");
        auto job = videos->create(Fixture::video(4), {});
        REQUIRE(job);
        fake.pushJson(200, R"({"id":"video_1","status":"completed","progress":100})");
        auto done = videos->poll(*job, {});
        REQUIRE(done);
        REQUIRE(done->outputUrls.size() == 1);
        CHECK(done->outputUrls[0] == provider.baseUrl + "/videos/video_1/content");
        fake.push(asset(kMp4, "video/mp4"));
        return videos->fetch(*done, directory, {});
    };
    auto local = runVideo(office, "");
    REQUIRE(local);
    CHECK(fake.requests().back().url == "http://127.0.0.1:8080/v1/videos/video_1/content");
    CHECK(contents(local->front()) == kMp4);

    ProviderSpec gateway = office;
    gateway.id = "custom-gateway";
    gateway.baseUrl = "https://ai.internal.example:8443/v1";
    gateway.auth = AuthScheme::Bearer;
    gateway.keyRequired = true;
    gateway.local = false;
    REQUIRE(runVideo(gateway, "fixture-only-gateway-key"));
    CHECK(fake.requests().back().url == "https://ai.internal.example:8443/v1/videos/video_1/content");
    CHECK(findHeader(fake.requests().back().headers, "Authorization") == "Bearer fixture-only-gateway-key");

    // Images answered with URLs: the server's own origin downloads; another port, host or scheme is refused unsent.
    ModelSpec image = video;
    image.id = "office-image";
    image.api = "openai-images";
    image.endpoint = "/images/generations";
    image.generation = {};
    image.generation.image = true;
    const auto runImage = [&](const ProviderSpec& provider, const std::string& url, bool served = false) {
        auto images = net::makeOpenAiImages(ProviderContext{provider, image, provider.baseUrl, Secret(), fake});
        fake.pushJson(200, Json{{"data", Json::array({Json{{"url", url}}})}}.dump());
        if (served) {
            fake.push(asset(kPng, "image/png")); // the download, after the submission's answer
        }
        auto ticket = images->create(Fixture::image(), {});
        REQUIRE(ticket);
        auto done = images->poll(*ticket, {});
        REQUIRE(done);
        return images->fetch(*done, directory, {});
    };
    auto png = runImage(office, "http://127.0.0.1:8080/generated-images/b64.png", true);
    REQUIRE(png);
    CHECK(png->front().extension() == ".png");
    CHECK(fake.requests().back().url == "http://127.0.0.1:8080/generated-images/b64.png");
    for (const char* elsewhere : {"http://127.0.0.1:9999/generated-images/b64.png", "http://10.0.0.5:8080/generated-images/b64.png",
                                  "https://127.0.0.1:8080/generated-images/b64.png", "http://user@127.0.0.1:8080/b64.png"}) {
        INFO(elsewhere);
        const std::size_t before = fake.requests().size();
        auto refused = runImage(office, elsewhere);
        REQUIRE_FALSE(refused);
        CHECK(refused.error().message.find("unapproved download host") != std::string::npos);
        CHECK(fake.requests().size() == before + 1); // the submission only: nothing was downloaded
    }
    // A plain-http server elsewhere on the network never serves a download.
    ProviderSpec lan = office;
    lan.baseUrl = "http://10.0.0.5:8080/v1";
    lan.local = false;
    REQUIRE_FALSE(runImage(lan, "http://10.0.0.5:8080/generated-images/b64.png"));
    // A catalogue cloud provider keeps the vendor rule: HTTPS on 443 on its own host.
    ProviderSpec catalogue = gateway;
    catalogue.id = "together-fixture";
    catalogue.custom = false;
    catalogue.baseUrl = "https://api.together.example/v1";
    REQUIRE_FALSE(runImage(catalogue, "https://api.together.example:8443/b64.png"));
    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
}

TEST_CASE("Custom providers make sound effects through a sound-generation path", "[ai][net][generation][r07][custom][sfx]") {
    // QA-01: OpenAI-compatible servers that make sound effects (LocalAI) take ElevenLabs' request at their own path and
    // answer with the audio file (WAV here); the file is kept by its signature and booked at the length that came back.
    FakeTransport fake;
    const std::filesystem::path directory = uniqueTempPath("wabcut-r07-custom-sfx");
    ProviderSpec office;
    office.id = "custom-office";
    office.name = "Office server";
    office.baseUrl = "http://127.0.0.1:8080/v1";
    office.auth = AuthScheme::None;
    office.keyRequired = false;
    office.custom = true;
    office.local = true;
    ModelSpec sfx;
    sfx.id = "musicgen-small";
    sfx.provider = office.id;
    sfx.name = "musicgen-small";
    sfx.api = "sound-generation";
    sfx.endpoint = "/sound-generation";
    sfx.caps.capabilities = {Capability::GenerateMedia};
    sfx.generation.soundEffect = true;
    sfx.generation.minSeconds = 0.5;
    sfx.generation.maxSeconds = 30.0;
    // 1 s of 8 kHz mono 8-bit PCM: byte rate 8000, 8000 data bytes.
    const std::string wav = std::string("RIFF", 4) + std::string("\x4C\x1F\x00\x00", 4) + "WAVEfmt " + std::string("\x10\x00\x00\x00", 4) +
                            std::string("\x01\x00\x01\x00\x40\x1F\x00\x00\x40\x1F\x00\x00\x01\x00\x08\x00", 16) + "data" +
                            std::string("\x40\x1F\x00\x00", 4) + std::string(8000, static_cast<char>(0x80));
    auto generator = net::makeCompatibleSoundEffects(ProviderContext{office, sfx, office.baseUrl, Secret(), fake});
    fake.push(asset(wav, "audio/wav"));
    MediaGenerationRequest request;
    request.kind = GeneratedMediaKind::SoundEffect;
    request.prompt = "footsteps on gravel";
    request.duration = Ticks::fromSeconds(2);
    auto ticket = generator->create(request, {});
    REQUIRE(ticket);
    const HttpRequest sent = fake.requests().at(0);
    CHECK(sent.url == "http://127.0.0.1:8080/v1/sound-generation");
    CHECK_FALSE(findHeader(sent.headers, "Authorization"));
    const Json body = Json::parse(sent.body);
    CHECK(body["model_id"] == "musicgen-small");
    CHECK(body["text"] == "footsteps on gravel");
    CHECK(body["duration_seconds"] == 2.0);
    auto done = generator->poll(*ticket, {});
    REQUIRE(done);
    CHECK(done->usage.audioMilliseconds == 1000);
    auto files = generator->fetch(*done, directory, {});
    REQUIRE(files);
    CHECK(files->front().extension() == ".wav");
    CHECK(contents(files->front()) == wav);

    // An HTML error page with a 200 is not a sound.
    fake.push(asset("<html>busy</html>", "text/html"));
    auto html = generator->create(request, {});
    REQUIRE_FALSE(html);
    CHECK(html.error().code == AiErrorCode::InvalidRequest);
    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
}
