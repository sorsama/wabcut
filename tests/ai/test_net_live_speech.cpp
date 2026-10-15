// AI-12: optional live smoke tests of the voice providers, each only when its key is in the environment:
//   WABCUT_LIVE_ELEVENLABS_KEY  WABCUT_LIVE_OPENAI_KEY  WABCUT_LIVE_GEMINI_KEY
// Without the variable a test passes without doing anything. CI never sets them; each costs a fraction of a cent (one
// short Thai line) and needs the network. The key is only handed to the adapter; it is never printed or logged (the
// request log redacts it). Run one with:  ctest --preset win-msvc -L ai_net -R "live speech" --output-on-failure

#include <wabcut/ai/Secrets.h>
#include <wabcut/ai_net/Adapters.h>
#include <wabcut/ai_net/Network.h>

#include <QCoreApplication>

#include <catch2/catch_test_macros.hpp>

using namespace wabcut::ai;

namespace {

void ensureCoreApp() {
    if (!QCoreApplication::instance()) {
        static int argc = 1;
        static char name[] = "wabcut_ai_net_tests";
        static char* argv[] = {name, nullptr};
        static QCoreApplication app(argc, argv);
    }
}

void liveSpeech(const char* variable, const std::string& modelId, const std::string& voice,
                std::unique_ptr<ISpeechSynth> (*make)(ProviderContext)) {
    auto key = systemEnv(variable);
    if (!key) {
        SUCCEED(std::string(variable) + " is not set: live test skipped");
        return;
    }
    ensureCoreApp();
    RequestLog log;
    net::NetworkStack stack(log);
    const ModelSpec& model = *ModelRegistry::builtin().model(modelId);
    const ProviderSpec& provider = *ModelRegistry::builtin().provider(model.provider);
    auto synth = make(ProviderContext{provider, model, provider.baseUrl, Secret(std::move(*key)), stack.transport()});
    SpeechRequest request;
    request.text = "สวัสดีครับ ยินดีต้อนรับ";
    request.voiceId = voice;
    request.language = "th";
    auto r = synth->synthesize(request, {});
    INFO((r ? std::string() : r.error().message));
    REQUIRE(r);
    CHECK(r->audio.size() > 1000);
    CHECK_FALSE(r->mediaType.empty());
}

} // namespace

TEST_CASE("Live speech: ElevenLabs v3 speaks Thai", "[ai][net][live][speech]") {
    liveSpeech("WABCUT_LIVE_ELEVENLABS_KEY", "eleven_v3", "JBFqnCBsd6RMkjVDRZzb", &net::makeElevenLabsSpeech);
}

TEST_CASE("Live speech: OpenAI gpt-4o-mini-tts speaks Thai", "[ai][net][live][speech]") {
    liveSpeech("WABCUT_LIVE_OPENAI_KEY", "gpt-4o-mini-tts", "marin", &net::makeOpenAiSpeech);
}

TEST_CASE("Live speech: Gemini 3.8 Flash TTS speaks Thai", "[ai][net][live][speech]") {
    liveSpeech("WABCUT_LIVE_GEMINI_KEY", "gemini-3.8-flash-tts", "Kore", &net::makeGeminiSpeech);
}
