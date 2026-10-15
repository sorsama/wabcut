// Review wave 4, P1-4: model and runtime downloads follow their CDN redirects on purpose. Hops are checked against an
// exact allow-list (Hugging Face -> its Xet/LFS hosts, GitHub -> its release-asset hosts), stay on HTTPS, never carry
// credentials to another host, and stop after a few hops. The loopback test of a real cross-origin 302 through
// QtHttpTransport is in test_net_qt_transport.cpp.

#include "FakeTransport.h"

#include <wabcut/ai/HttpDownload.h>
#include <wabcut/ai/Redirects.h>

#include <catch2/catch_test_macros.hpp>

using namespace wabcut;
using namespace wabcut::ai;
using wabcut::ai::test::FakeTransport;
using wabcut::ai::test::ScriptedResponse;

namespace {

ScriptedResponse redirect(int status, std::string location) {
    ScriptedResponse r;
    r.status = status;
    r.headers = {{"Location", std::move(location)}};
    return r;
}

ScriptedResponse bytes(std::string body) {
    ScriptedResponse r;
    r.status = 200;
    r.headers = {{"Content-Length", std::to_string(body.size())}};
    r.body = std::move(body);
    return r;
}

} // namespace

TEST_CASE("Downloads follow Hugging Face and GitHub CDN redirects without forwarding credentials", "[ai][net][redirect]") {
    FakeTransport transport;
    transport.push(redirect(302, "https://cas-bridge.xethub.hf.co/xet-bridge-us/abc?X-Amz-Signature=secret"));
    transport.push(bytes("GGUF-weights"));
    HttpDownloadTransport downloads(transport);
    std::string received;
    auto response = downloads.get(
        "https://huggingface.co/unsloth/model/resolve/rev/model.gguf", 0, {},
        [&](std::span<const std::byte> data) {
            received.append(reinterpret_cast<const char*>(data.data()), data.size());
            return true;
        },
        {});
    REQUIRE(response);
    CHECK(response->status == 200);
    CHECK(received == "GGUF-weights");
    const auto requests = transport.requests();
    REQUIRE(requests.size() == 2);
    CHECK_FALSE(requests[0].followRedirects); // the transport hands the 302 back instead of refusing it
    CHECK(requests[1].url.starts_with("https://cas-bridge.xethub.hf.co/"));

    // Hugging Face's CDN Xet bridge (2026-10).
    FakeTransport cdn;
    cdn.push(redirect(302, "https://us.aws.cdn.hf.co/xet-bridge-us/64f7/83c8?Signature=secret"));
    cdn.push(bytes("onnx"));
    HttpDownloadTransport cdnDownloads(cdn);
    auto viaCdn = cdnDownloads.get("https://huggingface.co/Xenova/opus-mt-en-mul/resolve/rev/onnx/encoder_model_quantized.onnx", 0, {},
                                   [](std::span<const std::byte>) { return true; }, {});
    REQUIRE(viaCdn);
    CHECK(cdn.requests().at(1).url.starts_with("https://us.aws.cdn.hf.co/"));

    // A resumed GitHub download keeps its Range across the hop, but no credentials.
    FakeTransport github;
    github.push(redirect(302, "https://release-assets.githubusercontent.com/github-production-release-asset/1?sig=x"));
    github.push(bytes("zip"));
    HttpRequest request;
    request.method = "GET";
    request.url = "https://github.com/ggml-org/llama.cpp/releases/download/b11429/llama.zip";
    request.headers = {{"Range", "bytes=10-"}, {"Authorization", "Bearer gh-secret"}, {"x-goog-api-key", "AIzaSecret"}, {"Cookie", "a=b"}};
    request.responseMode = BodyMode::Raw;
    std::vector<std::string> hops;
    auto followed = sendFollowingRedirects(github, request, {}, {}, RedirectAllowList::modelDownloads(), &hops);
    REQUIRE(followed);
    CHECK(hops == std::vector<std::string>{"release-assets.githubusercontent.com"});
    const auto sent = github.requests();
    REQUIRE(sent.size() == 2);
    CHECK(sent[1].headers == HttpHeaders{{"Range", "bytes=10-"}});
}

TEST_CASE("Redirects to unapproved hosts, downgrades, userinfo and endless chains fail closed", "[ai][net][redirect]") {
    const auto fails = [](ScriptedResponse first, std::string_view expected, int extra = 0) {
        FakeTransport t;
        t.push(std::move(first));
        for (int i = 0; i < extra; ++i) {
            t.push(redirect(302, "/again"));
        }
        t.push(bytes("never"));
        HttpRequest request;
        request.method = "GET";
        request.url = "https://huggingface.co/m/resolve/r/f.gguf";
        request.responseMode = BodyMode::Raw;
        auto result = sendFollowingRedirects(t, request, {}, {}, RedirectAllowList::modelDownloads());
        REQUIRE_FALSE(result);
        INFO(result.error().message);
        CHECK(result.error().code == AiErrorCode::ProviderError);
        CHECK(result.error().message.find(expected) != std::string::npos);
        CHECK(result.error().message.find("signature") == std::string::npos); // hosts only, never the signed URL
    };
    fails(redirect(302, "https://evil.example.com/steal?signature=x"), "evil.example.com");
    fails(redirect(302, "http://cas-bridge.xethub.hf.co/x"), "cas-bridge.xethub.hf.co");   // HTTPS only
    fails(redirect(302, "https://user:pw@cdn-lfs.hf.co/x"), "cdn-lfs.hf.co");               // no userinfo
    fails(redirect(302, "https://objects.githubusercontent.com/x"), "objects.githubusercontent.com"); // GitHub's, not HF's
    fails(redirect(302, "not a url"), "usable location");
    ScriptedResponse noLocation;
    noLocation.status = 302;
    fails(noLocation, "usable location");
    fails(redirect(302, "/again"), "too many redirects", 6);

    // The same host and relative locations are fine; 303 turns a POST into a GET without its body.
    FakeTransport t;
    t.push(redirect(303, "/result"));
    t.push(bytes("ok"));
    HttpRequest post;
    post.url = "https://huggingface.co/api/thing";
    post.body = "{}";
    auto result = sendFollowingRedirects(t, post, {}, {}, RedirectAllowList::sameHost());
    REQUIRE(result);
    CHECK(t.requests()[1].url == "https://huggingface.co/result");
    CHECK(t.requests()[1].method == "GET");
    CHECK(t.requests()[1].body.empty());
    CHECK(resolveLocation("https://a.example:8443/x/y", "/z") == "https://a.example:8443/z");
    CHECK(resolveLocation("https://a.example/x", "//b.example/q") == "https://b.example/q");
    CHECK_FALSE(resolveLocation("https://a.example/x", "relative/path"));
}
