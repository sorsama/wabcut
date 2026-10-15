// Optional paid smoke: requires an explicit dedicated live key AND WABCUT_LIVE_GENERATION=1; never runs in CI.
#include "FakeTransport.h"
#include <wabcut/ai/Provider.h>
#include <wabcut/ai/Secrets.h>
#include <wabcut/ai_net/Adapters.h>
#include <wabcut/ai_net/Network.h>
#include <QCoreApplication>
#include <catch2/catch_test_macros.hpp>
using namespace wabcut;
using namespace wabcut::ai;
TEST_CASE("Opt-in BYOK generation smoke with exact spend caps", "[ai][generation][live]") {
    if (systemEnv("CI") || systemEnv("WABCUT_LIVE_GENERATION") != std::optional<std::string>("1")) SKIP("paid smoke disabled");
    if (!QCoreApplication::instance()) {
        static int argc = 1; static char name[] = "wabcut_generation_live"; static char* argv[]{name, nullptr};
        static QCoreApplication app(argc, argv);
    }
    ModelRegistry models = ModelRegistry::builtin(); ProviderRegistry providers(models); net::registerCloudAdapters(providers);
    MemorySecretStore store; KeyRing keys(store); ConsentLedger consent; SpendGuard spend; RequestLog log;
    net::NetworkStack network(log); AiPlatform platform(providers, keys, consent, spend, log, network.transport());
    int ran = 0;
    for (const bool veo : {true, false}) {
        auto key = systemEnv(veo ? "WABCUT_LIVE_GEMINI_GENERATION_KEY" : "WABCUT_LIVE_RUNWAY_GENERATION_KEY");
        if (!key) continue;
        const std::string provider = veo ? "gemini" : "runway";
        REQUIRE(keys.save(*models.provider(provider), Secret(std::move(*key))));
        const MicroUsd cap = veo ? 400000 : 50000; spend.setMonthlyCap(provider, cap);
        MediaGenerationRequest request; request.prompt = "A still view of clouds over an empty mountain landscape";
        request.kind = veo ? GeneratedMediaKind::Video : GeneratedMediaKind::Image;
        if (veo) request.duration = Ticks::fromSeconds(4);
        auto dir = test::uniqueTempPath(veo ? "veo-live" : "runway-image-live");
        auto result = platform.generate(veo ? "veo-3.1-fast-generate-preview" : "gen4_image", request, dir, {}, {}, {true, cap});
        // No raw provider payload or key is printed, including on failure.
        REQUIRE(result); CHECK(result->cost <= cap); REQUIRE(result->files.size() == 1);
        CHECK(std::filesystem::file_size(result->files[0]) > 0); ++ran;
    }
    if (!ran) SKIP("no dedicated generation smoke keys");
}
