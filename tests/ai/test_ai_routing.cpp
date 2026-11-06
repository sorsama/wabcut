// r07 (A1): the AiController contract for Settings > AI: the features and what each uses now (routing), per-feature
// defaults (persisted, "automatic" = local first, then the first usable), custom providers (validated, persisted
// without their key, removed with their key, consents and defaults), a custom provider on a loopback address counting
// as local, and features that take their default from the routing (the Generate panel's model list). Keys stay in
// memory (WABCUT_AI_KEYSTORE=memory); nothing reaches the internet.

#include "ShellFixture.h"

#include "AgentController.h"
#include "AiController.h"
#include "GenerateController.h"

#include <wabcut/ai/llm/Tiers.h>

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QSettings>
#include <QStandardPaths>

using namespace wabcut::test;

namespace {

void isolate() {
    qputenv("WABCUT_AI_KEYSTORE", "memory");
    QStandardPaths::setTestModeEnabled(true);
    resetSettings();
    clearMessages();
}

QVariantMap find(const QVariantList& list, const QString& key, const QString& value) {
    for (const QVariant& v : list) {
        const QVariantMap m = v.toMap();
        if (m.value(key).toString() == value)
            return m;
    }
    return {};
}

QVariantMap route(const AiController& ai, const QString& feature) { return find(ai.routing(), QStringLiteral("capability"), feature); }

/// Every value QSettings holds (the INI file of this test process), as text.
QString allSettingsText() {
    QSettings settings;
    settings.sync();
    QString text;
    for (const QString& key : settings.allKeys())
        text += key + QLatin1Char('=') + settings.value(key).toString() + QLatin1Char('\n');
    QFile file(settings.fileName());
    if (file.open(QIODevice::ReadOnly))
        text += QString::fromUtf8(file.readAll());
    return text;
}

QVariantMap gatewaySpec() {
    return QVariantMap{
        {QStringLiteral("name"), QStringLiteral("My Gateway")},
        {QStringLiteral("protocol"), QStringLiteral("openai")},
        {QStringLiteral("baseUrl"), QStringLiteral("https://llm.example.test/v1/")},
        {QStringLiteral("auth"), QStringLiteral("header")},
        {QStringLiteral("authHeader"), QStringLiteral("x-gateway-key")},
        {QStringLiteral("headers"), QVariantMap{{QStringLiteral("X-Org"), QStringLiteral("studio-7")}}},
        {QStringLiteral("models"),
         QVariantList{QVariantMap{{QStringLiteral("id"), QStringLiteral("gw-chat")}, {QStringLiteral("capability"), QStringLiteral("chat")}},
                      // A name another provider already uses: kept, under a provider-scoped id.
                      QVariantMap{{QStringLiteral("id"), QStringLiteral("claude-sonnet-5-5")}, {QStringLiteral("capability"), QStringLiteral("chat")}}}},
        {QStringLiteral("paths"), QVariantMap{{QStringLiteral("chat"), QStringLiteral("/v2/chat/completions")}}},
    };
}

} // namespace

TEST_CASE("Routing gives every feature a row and follows its default", "[ai][ui][r07]") {
    isolate();
    {
        AiController ai;
        const QVariantList features = ai.capabilityList();
        REQUIRE(features.size() == 7);
        QStringList ids;
        for (const QVariant& f : features) {
            ids << f.toMap().value(QStringLiteral("id")).toString();
            CHECK_FALSE(f.toMap().value(QStringLiteral("name")).toString().isEmpty());
            CHECK_FALSE(f.toMap().value(QStringLiteral("description")).toString().isEmpty());
        }
        CHECK(ids == QStringList{"chat", "transcribe", "translate", "speak", "image", "video", "sound"});
        REQUIRE(ai.routing().size() == 7);

        // Nothing chosen and no key: the assistant is the local tier (automatic), the cloud-only features say why.
        const QVariantMap chat = route(ai, QStringLiteral("chat"));
        CHECK(chat.value(QStringLiteral("automatic")).toBool());
        CHECK(chat.value(QStringLiteral("local")).toBool());
        CHECK(ai.chatModel() == chat.value(QStringLiteral("model")).toString());
        const QVariantMap speak = route(ai, QStringLiteral("speak"));
        CHECK_FALSE(speak.value(QStringLiteral("usable")).toBool());
        CHECK_FALSE(speak.value(QStringLiteral("reason")).toString().isEmpty());
        CHECK(ai.routedModel(QStringLiteral("speak")).isEmpty());

        // Defaults are checked against the feature.
        CHECK_FALSE(ai.setDefaultModel(QStringLiteral("speak"), QStringLiteral("nova-3")));
        CHECK_FALSE(ai.setDefaultModel(QStringLiteral("telepathy"), QString()));
        CHECK_FALSE(ai.setDefaultModel(QStringLiteral("image"), QStringLiteral("no-such-model")));
        REQUIRE(ai.setDefaultModel(QStringLiteral("speak"), QStringLiteral("eleven_v3")));
        CHECK(ai.defaultModel(QStringLiteral("speak")) == QStringLiteral("eleven_v3"));
        QVariantMap row = route(ai, QStringLiteral("speak"));
        CHECK(row.value(QStringLiteral("model")) == QStringLiteral("eleven_v3"));
        CHECK_FALSE(row.value(QStringLiteral("automatic")).toBool());
        CHECK_FALSE(row.value(QStringLiteral("usable")).toBool());
        CHECK(row.value(QStringLiteral("reason")).toString().contains(QStringLiteral("ElevenLabs")));

        ai.saveKey(QStringLiteral("elevenlabs"), QStringLiteral("xi-routing-test-key-0123456789"), true);
        CHECK(route(ai, QStringLiteral("speak")).value(QStringLiteral("usable")).toBool());
        CHECK(ai.routedModel(QStringLiteral("speak")) == QStringLiteral("eleven_v3"));
        // Sound effects (automatic) find ElevenLabs now: the live registry, not a fixed list.
        const QVariantMap sound = route(ai, QStringLiteral("sound"));
        CHECK(sound.value(QStringLiteral("automatic")).toBool());
        CHECK(sound.value(QStringLiteral("usable")).toBool());
        CHECK(sound.value(QStringLiteral("provider")) == QStringLiteral("elevenlabs"));
        CHECK_FALSE(find(ai.modelsFor(QStringLiteral("image")), QStringLiteral("id"), QStringLiteral("gen4_image")).isEmpty());
        CHECK(find(ai.modelsFor(QStringLiteral("video")), QStringLiteral("id"), QStringLiteral("gen4_image")).isEmpty());

        // Translation follows the assistant until it has a model of its own.
        ai.saveKey(QStringLiteral("anthropic"), QStringLiteral("sk-ant-routing-test-0123456789"), true);
        REQUIRE(ai.setDefaultModel(QStringLiteral("chat"), QStringLiteral("claude-sonnet-5-5")));
        CHECK(ai.chatModel() == QStringLiteral("claude-sonnet-5-5"));
        row = route(ai, QStringLiteral("translate"));
        CHECK(row.value(QStringLiteral("automatic")).toBool());
        CHECK(row.value(QStringLiteral("model")) == QStringLiteral("claude-sonnet-5-5"));
        CHECK(row.value(QStringLiteral("usable")).toBool());
        REQUIRE(ai.setDefaultModel(QStringLiteral("translate"), QStringLiteral("gemini-3.8-flash")));
        row = route(ai, QStringLiteral("translate"));
        CHECK(row.value(QStringLiteral("model")) == QStringLiteral("gemini-3.8-flash"));
        CHECK_FALSE(row.value(QStringLiteral("usable")).toBool()); // no Gemini key
        // Image: a default the user picked is listed first by the Generate panel even before its key is there.
        REQUIRE(ai.setDefaultModel(QStringLiteral("image"), QStringLiteral("gen4_image")));
        wabcut::app::GenerateController generate;
        generate.setAiController(&ai);
        const QVariantList generation = generate.models();
        REQUIRE_FALSE(generation.isEmpty());
        CHECK(generation.front().toMap().value(QStringLiteral("id")) == QStringLiteral("gen4_image"));
        CHECK(generation.front().toMap().value(QStringLiteral("image")).toBool());

        // Offline: cloud choices say so.
        ai.setOfflineMode(true);
        CHECK(route(ai, QStringLiteral("speak")).value(QStringLiteral("reason")) == QStringLiteral("Offline"));
        ai.setOfflineMode(false);
    }
    {
        AiController again; // persisted
        CHECK(again.defaultModel(QStringLiteral("speak")) == QStringLiteral("eleven_v3"));
        CHECK(again.defaultModel(QStringLiteral("translate")) == QStringLiteral("gemini-3.8-flash"));
        CHECK(again.defaultModel(QStringLiteral("image")) == QStringLiteral("gen4_image"));
        CHECK(again.chatModel() == QStringLiteral("claude-sonnet-5-5"));
        REQUIRE(again.setDefaultModel(QStringLiteral("speak"), QString())); // back to automatic
        CHECK(again.defaultModel(QStringLiteral("speak")).isEmpty());
        const QVariantMap speak = route(again, QStringLiteral("speak"));
        CHECK(speak.value(QStringLiteral("automatic")).toBool());
        CHECK(speak.value(QStringLiteral("provider")) == QStringLiteral("elevenlabs")); // the voice provider with a key
    }
}

TEST_CASE("Custom providers are validated persisted without their key and removed cleanly", "[ai][ui][r07]") {
    isolate();
    QString id;
    {
        AiController ai;
        const auto refused = [&](QVariantMap spec, const QString& fragment) {
            const QVariantMap r = ai.saveCustomProvider(spec);
            CHECK_FALSE(r.value(QStringLiteral("ok")).toBool());
            const QString error = r.value(QStringLiteral("error")).toString();
            INFO(error.toStdString());
            CHECK(error.contains(fragment, Qt::CaseInsensitive));
        };
        QVariantMap spec = gatewaySpec();
        spec[QStringLiteral("name")] = QString();
        refused(spec, QStringLiteral("name"));
        spec = gatewaySpec();
        spec[QStringLiteral("baseUrl")] = QStringLiteral("ftp://llm.example.test");
        refused(spec, QStringLiteral("address"));
        spec[QStringLiteral("baseUrl")] = QStringLiteral("https://user:secret@llm.example.test/v1");
        refused(spec, QStringLiteral("key field"));
        spec = gatewaySpec();
        spec[QStringLiteral("headers")] = QVariantMap{{QStringLiteral("Authorization"), QStringLiteral("Bearer abc")}};
        refused(spec, QStringLiteral("key field"));
        spec = gatewaySpec();
        spec[QStringLiteral("headers")] = QVariantMap{{QStringLiteral("X-Bad"), QStringLiteral("one\ntwo")}};
        refused(spec, QStringLiteral("one line"));
        spec = gatewaySpec();
        spec[QStringLiteral("paths")] = QVariantMap{{QStringLiteral("chat"), QStringLiteral("https://elsewhere.test/x")}};
        refused(spec, QStringLiteral("start with /"));
        spec = gatewaySpec();
        spec[QStringLiteral("protocol")] = QStringLiteral("corba");
        refused(spec, QStringLiteral("kind of API"));
        spec = gatewaySpec();
        spec[QStringLiteral("protocol")] = QStringLiteral("anthropic");
        spec[QStringLiteral("models")] = QVariantList{QVariantMap{{QStringLiteral("id"), QStringLiteral("w")}, {QStringLiteral("capability"), QStringLiteral("transcribe")}}};
        refused(spec, QStringLiteral("cannot"));
        spec = gatewaySpec();
        spec[QStringLiteral("id")] = QStringLiteral("anthropic"); // not a custom provider
        refused(spec, QStringLiteral("no longer exists"));
        for (const QVariant& p : ai.providers()) { // nothing was added by the refused specs
            CHECK_FALSE(p.toMap().value(QStringLiteral("custom")).toBool());
        }

        const QVariantMap saved = ai.saveCustomProvider(gatewaySpec());
        REQUIRE(saved.value(QStringLiteral("ok")).toBool());
        id = saved.value(QStringLiteral("id")).toString();
        CHECK(id == QStringLiteral("custom-my-gateway"));
        QVariantMap row = find(ai.providers(), QStringLiteral("id"), id);
        REQUIRE_FALSE(row.isEmpty());
        CHECK(row.value(QStringLiteral("category")) == QStringLiteral("custom"));
        CHECK(row.value(QStringLiteral("custom")).toBool());
        CHECK(row.value(QStringLiteral("status")) == QStringLiteral("needsKey"));
        CHECK(row.value(QStringLiteral("baseUrl")) == QStringLiteral("https://llm.example.test/v1"));
        CHECK(row.value(QStringLiteral("modelCount")).toInt() == 2);
        CHECK(row.value(QStringLiteral("capabilities")).toStringList().contains(QStringLiteral("chat")));
        // The clashing name got a provider-scoped id; Anthropic's model is untouched.
        const QVariantMap scoped = find(ai.modelsFor(QStringLiteral("chat")), QStringLiteral("id"), id + QStringLiteral(":claude-sonnet-5-5"));
        CHECK(scoped.value(QStringLiteral("name")) == QStringLiteral("claude-sonnet-5-5"));
        CHECK(scoped.value(QStringLiteral("price")) == QStringLiteral("Price not tracked"));
        CHECK(find(ai.modelsFor(QStringLiteral("chat")), QStringLiteral("id"), QStringLiteral("claude-sonnet-5-5")).value(QStringLiteral("provider")) ==
              QStringLiteral("anthropic"));
        // A second provider with the same name gets its own id.
        const QVariantMap twin = ai.saveCustomProvider(QVariantMap{{QStringLiteral("name"), QStringLiteral("My Gateway")},
                                                                   {QStringLiteral("baseUrl"), QStringLiteral("https://other.example.test/v1")}});
        REQUIRE(twin.value(QStringLiteral("ok")).toBool());
        CHECK(twin.value(QStringLiteral("id")) == QStringLiteral("custom-my-gateway-2"));
        CHECK(ai.removeCustomProvider(twin.value(QStringLiteral("id")).toString()));

        // Editing data never includes a key; the key goes through saveKey and stays out of settings.
        QVariantMap edit = ai.customProvider(id);
        CHECK(edit.value(QStringLiteral("name")) == QStringLiteral("My Gateway"));
        CHECK(edit.value(QStringLiteral("authHeader")) == QStringLiteral("x-gateway-key"));
        CHECK(edit.value(QStringLiteral("models")).toList().size() == 2);
        CHECK_FALSE(edit.value(QStringLiteral("hasKey")).toBool());
        ai.saveKey(id, QStringLiteral("gateway-secret-0123456789"), true);
        CHECK(find(ai.providers(), QStringLiteral("id"), id).value(QStringLiteral("status")) == QStringLiteral("ready"));
        CHECK(ai.customProvider(id).value(QStringLiteral("hasKey")).toBool());
        CHECK_FALSE(allSettingsText().contains(QStringLiteral("gateway-secret")));
        CHECK(allSettingsText().contains(QStringLiteral("x-gateway-key"))); // the spec itself is persisted

        // A cloud custom provider asks for consent like any other, and serves the default the user gives it.
        const QVariantMap prompt = ai.consentPrompt(QStringLiteral("gw-chat"), QStringLiteral("chat"), QStringLiteral("text"));
        CHECK(prompt.value(QStringLiteral("needed")).toBool());
        CHECK(prompt.value(QStringLiteral("destination")) == QStringLiteral("llm.example.test"));
        REQUIRE(ai.setDefaultModel(QStringLiteral("chat"), QStringLiteral("gw-chat")));
        CHECK(route(ai, QStringLiteral("chat")).value(QStringLiteral("usable")).toBool());
        CHECK(ai.platform().resolve(wabcut::ai::Capability::Chat).value().front().model->id == "gw-chat");

        // Models added later join the provider's spec.
        CHECK(ai.addModel(id, QStringLiteral("gw-voice"), QStringLiteral("speak")));
        CHECK(find(ai.modelsFor(QStringLiteral("speak")), QStringLiteral("id"), QStringLiteral("gw-voice")).value(QStringLiteral("provider")) == id);
        CHECK(ai.customProvider(id).value(QStringLiteral("models")).toList().size() == 3);
        CHECK(ai.removeModel(QStringLiteral("gw-voice")));
        CHECK(ai.customProvider(id).value(QStringLiteral("models")).toList().size() == 2);
    }
    {
        AiController again; // loaded from settings, validated again
        const QVariantMap row = find(again.providers(), QStringLiteral("id"), id);
        REQUIRE_FALSE(row.isEmpty());
        CHECK(row.value(QStringLiteral("modelCount")).toInt() == 2);
        CHECK(again.defaultModel(QStringLiteral("chat")) == QStringLiteral("gw-chat"));
        CHECK(again.customProvider(id).value(QStringLiteral("headers")).toMap().value(QStringLiteral("X-Org")) == QStringLiteral("studio-7"));
        REQUIRE(again.removeCustomProvider(id));
        CHECK(find(again.providers(), QStringLiteral("id"), id).isEmpty());
        CHECK(again.defaultModel(QStringLiteral("chat")).isEmpty()); // back to automatic
        CHECK(again.chatModel() != QStringLiteral("gw-chat"));
        CHECK_FALSE(QSettings().contains(QStringLiteral("ai/keys/%1/hasKey").arg(id)));
        CHECK_FALSE(again.removeCustomProvider(id));
        CHECK_FALSE(again.removeCustomProvider(QStringLiteral("anthropic"))); // built-in providers stay
    }
    {
        AiController third;
        CHECK(find(third.providers(), QStringLiteral("id"), id).isEmpty());
    }
}

TEST_CASE("Tampered custom provider settings are dropped", "[ai][ui][r07]") {
    isolate();
    // Not inside QStringLiteral: MSVC splits a multi-line raw string in a macro argument.
    const char* tampered = R"json([{"id":"custom-ok","name":"OK","baseUrl":"http://127.0.0.1:8000/v1","auth":"none",
                                 "models":[{"id":"ok-model","capability":"chat"}]},
                                {"id":"custom-bad","name":"Bad","baseUrl":"javascript:alert(1)"},
                                {"id":"anthropic","name":"Hijack","baseUrl":"https://evil.test/v1"},
                                {"id":"custom-hdr","name":"Hdr","baseUrl":"https://x.test/v1","headers":{"Authorization":"Bearer leaked"}},
                                42])json";
    QSettings().setValue(QStringLiteral("ai/customProviders"), QString::fromUtf8(tampered));
    AiController ai;
    CHECK_FALSE(find(ai.providers(), QStringLiteral("id"), QStringLiteral("custom-ok")).isEmpty());
    CHECK(find(ai.providers(), QStringLiteral("id"), QStringLiteral("custom-bad")).isEmpty());
    CHECK(find(ai.providers(), QStringLiteral("id"), QStringLiteral("custom-hdr")).isEmpty());
    CHECK(find(ai.providers(), QStringLiteral("id"), QStringLiteral("anthropic")).value(QStringLiteral("name")) == QStringLiteral("Anthropic"));
}

TEST_CASE("A custom provider on a loopback address is local", "[ai][ui][r07]") {
    isolate();
    AiController ai;
    const QVariantMap saved = ai.saveCustomProvider(QVariantMap{
        {QStringLiteral("name"), QStringLiteral("Office server")},
        {QStringLiteral("baseUrl"), QStringLiteral("http://127.0.0.1:8123/v1")},
        {QStringLiteral("auth"), QStringLiteral("none")},
        {QStringLiteral("models"), QVariantList{QVariantMap{{QStringLiteral("id"), QStringLiteral("office-llm")}, {QStringLiteral("capability"), QStringLiteral("chat")}}}}});
    REQUIRE(saved.value(QStringLiteral("ok")).toBool());
    const QString id = saved.value(QStringLiteral("id")).toString();
    const QVariantMap row = find(ai.providers(), QStringLiteral("id"), id);
    CHECK(row.value(QStringLiteral("category")) == QStringLiteral("custom"));
    CHECK(row.value(QStringLiteral("status")) == QStringLiteral("local"));
    CHECK_FALSE(ai.consentPrompt(QStringLiteral("office-llm"), QStringLiteral("chat"), QStringLiteral("text")).value(QStringLiteral("needed")).toBool());
    // Automatic: a local model that can run now comes before any cloud model (and before a tier still to download).
    CHECK(route(ai, QStringLiteral("chat")).value(QStringLiteral("local")).toBool());
    // Offline and no-cloud do not stop it.
    ai.setOfflineMode(true);
    ai.setProjectLock(true);
    REQUIRE(ai.setDefaultModel(QStringLiteral("chat"), QStringLiteral("office-llm")));
    const QVariantMap chat = route(ai, QStringLiteral("chat"));
    CHECK(chat.value(QStringLiteral("usable")).toBool());
    CHECK(chat.value(QStringLiteral("local")).toBool());
    CHECK(ai.statusLabel() == QStringLiteral("Offline"));
    ai.setProjectLock(false);
    ai.setOfflineMode(false);
    CHECK(ai.statusLabel() == QStringLiteral("Local"));
}

TEST_CASE("Listing a provider's models says why it cannot", "[ai][ui][r07]") {
    isolate();
    AiController ai;
    QString provider;
    bool ok = true;
    QString message;
    int answers = 0;
    QObject::connect(&ai, &AiController::remoteModelsListed, &ai,
                     [&](const QString& p, bool success, const QVariantList&, const QString& text) {
                         provider = p;
                         ok = success;
                         message = text;
                         ++answers;
                     });
    ai.listRemoteModels(QStringLiteral("no-such-provider"));
    CHECK(answers == 1);
    CHECK_FALSE(ok);
    ai.listRemoteModels(QStringLiteral("together")); // no key yet
    CHECK(answers == 2);
    CHECK_FALSE(ok);
    CHECK(message.contains(QStringLiteral("key")));
    ai.listRemoteModels(QStringLiteral("azure-openai")); // its address still has a placeholder
    CHECK(answers == 3);
    CHECK(message.contains(QStringLiteral("address")));
    // A loopback server that is not running: answered off the GUI thread, through the network stack (never the internet).
    const QVariantMap saved = ai.saveCustomProvider(QVariantMap{{QStringLiteral("name"), QStringLiteral("Nothing here")},
                                                                {QStringLiteral("baseUrl"), QStringLiteral("http://127.0.0.1:9/v1")},
                                                                {QStringLiteral("auth"), QStringLiteral("none")}});
    REQUIRE(saved.value(QStringLiteral("ok")).toBool());
    ai.listRemoteModels(saved.value(QStringLiteral("id")).toString());
    QElapsedTimer timer;
    timer.start();
    while (answers < 4 && timer.elapsed() < 60'000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    REQUIRE(answers == 4);
    CHECK(provider == saved.value(QStringLiteral("id")).toString());
    CHECK_FALSE(ok);
    CHECK_FALSE(message.isEmpty());
}

TEST_CASE("Custom OpenAI-compatible providers make sound effects through a sound-generation path", "[ai][ui][r07][custom]") {
    // QA-01: OpenAI's API has no sound-effects endpoint; servers that make them (LocalAI) serve ElevenLabs' request at
    // /v1/sound-generation, so OpenAI-compatible custom providers offer sound effects there (the path is editable).
    isolate();
    AiController ai;
    QStringList openAi;
    QStringList anthropic;
    for (const QVariant& p : ai.customProtocols()) {
        const QVariantMap m = p.toMap();
        (m.value(QStringLiteral("id")) == QStringLiteral("openai") ? openAi : anthropic) += m.value(QStringLiteral("capabilities")).toStringList();
    }
    CHECK(openAi.contains(QStringLiteral("sound")));
    CHECK_FALSE(anthropic.contains(QStringLiteral("sound")));
    const QVariantMap saved = ai.saveCustomProvider(QVariantMap{
        {QStringLiteral("name"), QStringLiteral("Studio sounds")},
        {QStringLiteral("baseUrl"), QStringLiteral("http://127.0.0.1:8080/v1")},
        {QStringLiteral("auth"), QStringLiteral("none")},
        {QStringLiteral("models"), QVariantList{QVariantMap{{QStringLiteral("id"), QStringLiteral("audioldm2")}, {QStringLiteral("capability"), QStringLiteral("sound")}}}}});
    INFO(saved.value(QStringLiteral("error")).toString().toStdString());
    REQUIRE(saved.value(QStringLiteral("ok")).toBool());
    const QString id = saved.value(QStringLiteral("id")).toString();
    CHECK(find(ai.providers(), QStringLiteral("id"), id).value(QStringLiteral("capabilities")).toStringList().contains(QStringLiteral("sound")));
    const QVariantMap sfx = find(ai.modelsFor(QStringLiteral("sound")), QStringLiteral("id"), QStringLiteral("audioldm2"));
    CHECK(sfx.value(QStringLiteral("provider")) == id);
    CHECK(sfx.value(QStringLiteral("usable")).toBool());
    CHECK(sfx.value(QStringLiteral("local")).toBool());
    const wabcut::ai::ModelSpec* model = ai.platform().providers().models().model("audioldm2");
    REQUIRE(model != nullptr);
    CHECK(model->api == "sound-generation");
    CHECK(model->endpoint == "/sound-generation");
    CHECK(model->generation.soundEffect);
    CHECK(model->generation.maxSeconds > 0.0);
    REQUIRE(ai.setDefaultModel(QStringLiteral("sound"), QStringLiteral("audioldm2")));
    CHECK(route(ai, QStringLiteral("sound")).value(QStringLiteral("usable")).toBool());

    // The Generate panel lists it as a sound model (the routed default first) and, QA-06, prices it offline and under
    // the no-cloud lock: a server on this computer is not the cloud.
    wabcut::app::GenerateController generate;
    generate.setAiController(&ai);
    const QVariantList generation = generate.models();
    REQUIRE_FALSE(generation.isEmpty());
    const QVariantMap first = find(generation, QStringLiteral("id"), QStringLiteral("audioldm2"));
    CHECK(first.value(QStringLiteral("sound")).toBool());
    ai.setOfflineMode(true);
    ai.setProjectLock(true);
    const QVariantMap quote = generate.estimate(QStringLiteral("audioldm2"), QStringLiteral("rain on a tent"), 5, QStringLiteral("16:9"), false);
    INFO(quote.value(QStringLiteral("error")).toString().toStdString());
    CHECK(quote.value(QStringLiteral("ok")).toBool());
    const QVariantMap cloud = generate.estimate(QStringLiteral("eleven_text_to_sound_v2"), QStringLiteral("rain"), 5, QStringLiteral("16:9"), false);
    CHECK_FALSE(cloud.value(QStringLiteral("ok")).toBool());
    ai.setProjectLock(false);
    ai.setOfflineMode(false);

    // Anthropic-compatible providers make no sound effects: refused, never a dead option.
    const QVariantMap refused = ai.saveCustomProvider(QVariantMap{
        {QStringLiteral("name"), QStringLiteral("Claude gateway")},
        {QStringLiteral("protocol"), QStringLiteral("anthropic")},
        {QStringLiteral("baseUrl"), QStringLiteral("https://gw.example.test")},
        {QStringLiteral("models"), QVariantList{QVariantMap{{QStringLiteral("id"), QStringLiteral("sfx")}, {QStringLiteral("capability"), QStringLiteral("sound")}}}}});
    CHECK_FALSE(refused.value(QStringLiteral("ok")).toBool());
}

TEST_CASE("OpenAI-compatible hosts take model ids for every feature", "[ai][ui][r07]") {
    // QA-05: gateways and servers of the user's (LiteLLM, LocalAI, vLLM) and OpenAI itself (Sora) ship no model of some
    // kinds; a model id the user adds for one speaks the OpenAI path of that kind on the host.
    isolate();
    {
        AiController ai;
        REQUIRE(ai.addModel(QStringLiteral("litellm"), QStringLiteral("whisper-proxy"), QStringLiteral("transcribe")));
        REQUIRE(ai.addModel(QStringLiteral("localai"), QStringLiteral("sd-turbo"), QStringLiteral("image")));
        REQUIRE(ai.addModel(QStringLiteral("localai"), QStringLiteral("audioldm2"), QStringLiteral("sound")));
        REQUIRE(ai.addModel(QStringLiteral("localai"), QStringLiteral("kokoro"), QStringLiteral("speak")));
        REQUIRE(ai.addModel(QStringLiteral("openai"), QStringLiteral("sora-2"), QStringLiteral("video")));
        const auto& registry = ai.platform().providers().models();
        REQUIRE(registry.model("whisper-proxy") != nullptr);
        CHECK(registry.model("whisper-proxy")->api == "openai-transcriptions");
        CHECK(registry.model("whisper-proxy")->endpoint == "/audio/transcriptions");
        CHECK(registry.model("sd-turbo")->generation.image);
        CHECK(registry.model("audioldm2")->generation.soundEffect);
        CHECK(registry.model("kokoro")->api == "openai-speech");
        CHECK(registry.model("sora-2")->api == "openai-videos");
        CHECK(find(ai.modelsFor(QStringLiteral("video")), QStringLiteral("id"), QStringLiteral("sora-2")).value(QStringLiteral("provider")) ==
              QStringLiteral("openai"));
        CHECK(find(ai.modelsFor(QStringLiteral("transcribe")), QStringLiteral("id"), QStringLiteral("whisper-proxy")).value(QStringLiteral("provider")) ==
              QStringLiteral("litellm"));
        // A server on this computer bills nothing; a cloud provider's added model has no price Wabcut knows.
        CHECK(registry.model("sd-turbo")->prices.size() == 1);
        CHECK(registry.model("sora-2")->prices.empty());
        // "Add model id" offers those kinds for OpenAI-compatible hosts, not for a vendor API of one kind.
        const QStringList addable = find(ai.providers(), QStringLiteral("id"), QStringLiteral("litellm")).value(QStringLiteral("addableCapabilities")).toStringList();
        for (const char* kind : {"chat", "transcribe", "speak", "image", "video", "sound"}) {
            INFO(kind);
            CHECK(addable.contains(QLatin1String(kind)));
        }
        CHECK_FALSE(find(ai.providers(), QStringLiteral("id"), QStringLiteral("deepgram")).value(QStringLiteral("addableCapabilities")).toStringList().contains(QStringLiteral("image")));
        CHECK_FALSE(ai.addModel(QStringLiteral("deepgram"), QStringLiteral("not-an-image-model"), QStringLiteral("image")));
    }
    {
        AiController again; // persisted with their feature
        CHECK(find(again.modelsFor(QStringLiteral("sound")), QStringLiteral("id"), QStringLiteral("audioldm2")).value(QStringLiteral("provider")) ==
              QStringLiteral("localai"));
        CHECK(find(again.modelsFor(QStringLiteral("video")), QStringLiteral("id"), QStringLiteral("sora-2")).value(QStringLiteral("provider")) ==
              QStringLiteral("openai"));
    }
}

TEST_CASE("The assistant's data flow follows the address the user set", "[ai][ui][r07]") {
    // QA-11: Ollama moved to a server elsewhere is the cloud: the panel names that host and no longer says "this
    // computer" or "No cost (local)".
    isolate();
    AiController ai;
    REQUIRE(ai.addModel(QStringLiteral("ollama"), QStringLiteral("llama-test"), QStringLiteral("chat")));
    REQUIRE(ai.setDefaultModel(QStringLiteral("chat"), QStringLiteral("llama-test")));
    wabcut::app::AgentController agent;
    agent.setAiController(&ai);
    QVariantMap flow = agent.dataFlow();
    CHECK(flow.value(QStringLiteral("local")).toBool());
    CHECK(flow.value(QStringLiteral("consent")) == QStringLiteral("local"));
    REQUIRE(ai.setBaseUrl(QStringLiteral("ollama"), QStringLiteral("https://llm.example.test/v1")));
    flow = agent.dataFlow();
    CHECK_FALSE(flow.value(QStringLiteral("local")).toBool());
    CHECK(flow.value(QStringLiteral("destination")) == QStringLiteral("llm.example.test"));
    CHECK(flow.value(QStringLiteral("consent")) == QStringLiteral("needed"));
}
