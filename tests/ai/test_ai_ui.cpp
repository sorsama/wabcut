// AI platform UI (owner: AI platform engineer): the AiController bridge (display values only, keys never in
// settings, persistence) and Preferences > AI screenshots rendered offscreen from the real Main.qml through the
// Settings popover entry. Uses the UI shell fixture of tests/app (unchanged) and keeps keys in memory
// (WABCUT_AI_KEYSTORE=memory), so the developer's keychain is never touched.

#include "ShellFixture.h"

#include "AiController.h"
#include "GenerateController.h"
#include "ProjectController.h" // complete for CreatorBridge.h's inline setController
#include "CreatorBridge.h"
#include "GlobalBridge.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include <wabcut/ai/ChildProcess.h>
#include <wabcut/ai/Secrets.h>
#include <wabcut/ai/llm/Tiers.h>

#include <QCoreApplication>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QThread>
#include <QFile>
#include <QMetaProperty>
#include <QQmlComponent>
#include <QSettings>
#include <QStandardPaths>

using namespace wabcut::test;

namespace {

const QColor kViolet(0x9B, 0x6C, 0xFF); // design language v3 violet, the default accent
constexpr char kSecret[] = "sk-ant-ui-test-key-0123456789abcd";

void isolate() {
    qputenv("WABCUT_AI_KEYSTORE", "memory");
    QStandardPaths::setTestModeEnabled(true); // the request log goes to the test location
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

/// Every value QSettings holds (the INI files of this test process), as text.
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

} // namespace

TEST_CASE("AiController exposes display values and never stores keys in settings", "[ai][ui]") {
    isolate();
    {
        AiController ai;
        CHECK_FALSE(ai.keyStorePersistent());
        CHECK(ai.keyStoreName() == QStringLiteral("this session only"));
        CHECK(ai.unverifiedEntries() == QStringList{QStringLiteral("model:voxtral-mini-latest")});
        CHECK(ai.registryUpdated() == QStringLiteral("2026-10-12"));

        QVariantMap anthropic = find(ai.providers(), QStringLiteral("id"), QStringLiteral("anthropic"));
        REQUIRE_FALSE(anthropic.isEmpty());
        CHECK(anthropic.value(QStringLiteral("status")) == QStringLiteral("none"));
        CHECK(anthropic.value(QStringLiteral("termsUrl")).toString().startsWith(QStringLiteral("https://")));
        CHECK(find(ai.providers(), QStringLiteral("id"), QStringLiteral("ollama")).value(QStringLiteral("status")) == QStringLiteral("local"));

        bool finished = false;
        bool ok = false;
        QObject::connect(&ai, &AiController::keyCheckFinished, [&](const QString& id, bool success, const QString&) {
            finished = id == QStringLiteral("anthropic");
            ok = success;
        });
        ai.saveKey(QStringLiteral("anthropic"), QString::fromLatin1(kSecret), /*skipCheck=*/true);
        CHECK(finished);
        CHECK(ok);
        anthropic = find(ai.providers(), QStringLiteral("id"), QStringLiteral("anthropic"));
        CHECK(anthropic.value(QStringLiteral("hasKey")).toBool());
        CHECK(anthropic.value(QStringLiteral("masked")) == QStringLiteral("••••abcd"));
        CHECK(anthropic.value(QStringLiteral("statusText")).toString().contains(QStringLiteral("abcd")));
        CHECK_FALSE(allSettingsText().contains(QStringLiteral("sk-ant-ui-test")));
        CHECK(ai.platform().keys().get(*wabcut::ai::ModelRegistry::builtin().provider("anthropic"))->reveal() == kSecret);

        const QVariantMap prompt = ai.consentPrompt(QStringLiteral("scribe_v2"), QStringLiteral("transcribe"),
                                                    QStringLiteral("audio of 3 clips, 12 min"));
        CHECK(prompt.value(QStringLiteral("needed")).toBool());
        CHECK(prompt.value(QStringLiteral("payload")) == QStringLiteral("audio of 3 clips, 12 min"));
        CHECK(prompt.value(QStringLiteral("termsUrl")).toString().contains(QStringLiteral("elevenlabs.io")));

        // Model choices, privacy and caps are persisted.
        // AI-15: until one is picked, the local tier this computer runs best (which one depends on the machine).
        CHECK(wabcut::ai::llm::tierForModel(ai.chatModel().toStdString()) != nullptr);
        CHECK(ai.transcriptionModel().isEmpty()); // on this computer by default
        ai.setChatModel(QStringLiteral("gpt-6.1-sol"));
        ai.setTranscriptionModel(QStringLiteral("nova-3"));
        CHECK(ai.statusLabel() == QStringLiteral("OpenAI"));
        ai.setOfflineMode(true);
        spin(50); // ledger callbacks are queued to the GUI thread
        CHECK(ai.statusLabel() == QStringLiteral("Offline"));
        ai.setMonthlyCap(QStringLiteral("anthropic"), 20.0);
        spin(50);
        const QVariantMap spend = find(ai.spend(), QStringLiteral("provider"), QStringLiteral("anthropic"));
        CHECK(spend.value(QStringLiteral("cap")) == QStringLiteral("$20.00"));
        CHECK(spend.value(QStringLiteral("spent")) == QStringLiteral("$0.00"));
        ai.grantConsent(QStringLiteral("deepgram"), QStringLiteral("transcribe"));
        spin(50);
        REQUIRE(ai.consents().size() == 1);
        CHECK(ai.consents().first().toMap().value(QStringLiteral("capabilityName")) == QStringLiteral("Transcription"));

        // Model rows explain why a model cannot run.
        const QVariantMap gemini = find(ai.chatModels(), QStringLiteral("id"), QStringLiteral("gemini-3.8-flash"));
        CHECK_FALSE(gemini.value(QStringLiteral("usable")).toBool());
        CHECK(gemini.value(QStringLiteral("badges")).toStringList().contains(QStringLiteral("Video")));
        CHECK(gemini.value(QStringLiteral("price")) == QStringLiteral("$0.75 in · $3.75 out per 1M tokens"));
        CHECK(find(ai.transcriptionModels(), QStringLiteral("id"), QStringLiteral("nova-3")).value(QStringLiteral("price")) ==
              QStringLiteral("$0.258 per hour"));

        // User-defined local models.
        CHECK(ai.addModel(QStringLiteral("ollama"), QStringLiteral("qwen3.5:9b")));
        CHECK_FALSE(ai.addModel(QStringLiteral("ollama"), QStringLiteral("qwen3.5:9b")));
        CHECK_FALSE(find(ai.chatModels(), QStringLiteral("id"), QStringLiteral("qwen3.5:9b")).isEmpty());
        CHECK(ai.setBaseUrl(QStringLiteral("ollama"), QStringLiteral("http://127.0.0.1:11500/v1")));
        CHECK_FALSE(ai.setBaseUrl(QStringLiteral("ollama"), QStringLiteral("ftp://nope")));

        const QVariantMap blocked = ai.consentPrompt(QStringLiteral("scribe_v2"), QStringLiteral("transcribe"),
                                                     QStringLiteral("audio of 3 clips, 12 min"));
        CHECK(blocked.value(QStringLiteral("destination")) == QStringLiteral("api.elevenlabs.io"));
        CHECK_FALSE(blocked.value(QStringLiteral("needed")).toBool()); // offline mode is on: blocked, not asked
        CHECK(blocked.value(QStringLiteral("cost")) == QStringLiteral("$0.22002 per hour"));

        // Principle 9: no raw 64-bit integers cross into QML.
        const QMetaObject* meta = ai.metaObject();
        for (int i = meta->propertyOffset(); i < meta->propertyCount(); ++i) {
            const QMetaType type = meta->property(i).metaType();
            INFO(meta->property(i).name());
            CHECK(type.id() != QMetaType::LongLong);
            CHECK(type.id() != QMetaType::ULongLong);
        }
    }

    // A new controller (next start) restores everything but the key, which lived in this session's memory store.
    AiController again;
    CHECK(again.chatModel() == QStringLiteral("gpt-6.1-sol"));
    CHECK(again.transcriptionModel() == QStringLiteral("nova-3"));
    CHECK(again.offlineMode());
    CHECK(find(again.spend(), QStringLiteral("provider"), QStringLiteral("anthropic")).value(QStringLiteral("cap")) ==
          QStringLiteral("$20.00"));
    CHECK(again.consents().size() == 1);
    CHECK_FALSE(find(again.chatModels(), QStringLiteral("id"), QStringLiteral("qwen3.5:9b")).isEmpty());
    CHECK(find(again.providers(), QStringLiteral("id"), QStringLiteral("ollama")).value(QStringLiteral("baseUrl")) ==
          QStringLiteral("http://127.0.0.1:11500/v1"));
    again.removeKey(QStringLiteral("anthropic"));
    CHECK_FALSE(find(again.providers(), QStringLiteral("id"), QStringLiteral("anthropic")).value(QStringLiteral("hasKey")).toBool());
}

TEST_CASE("Preferences > AI screenshots", "[ai][ui][screenshot]") {
    struct Shot {
        const char* file;
        int tab;
    };
    const Shot shots[] = {
        {"preferences-ai-providers.png", 0},
        {"preferences-ai-models.png", 1},
        {"preferences-ai-privacy.png", 2},
        {"preferences-ai-usage.png", 3},
    };
    for (const Shot& shot : shots) {
        INFO(shot.file);
        isolate();
        Shell shell;
        REQUIRE(shell.window != nullptr);
        INFO(join(shell.errors));
        shell.resize(kShotWidth, kShotHeight);

        // Content so every page shows real rows: a stored key, a consent, caps and some request-log lines.
        auto* ai = shell.engine.singletonInstance<AiController*>("Wabcut", "AiController");
        REQUIRE(ai != nullptr);
        ai->saveKey(QStringLiteral("anthropic"), QString::fromLatin1(kSecret), true);
        ai->addModel(QStringLiteral("ollama"), QStringLiteral("qwen3.5:9b"));
        ai->grantConsent(QStringLiteral("deepgram"), QStringLiteral("transcribe"));
        ai->setMonthlyCap(QStringLiteral("anthropic"), 25.0);
        ai->setMonthlyCap(QStringLiteral("deepgram"), 10.0);
        auto& log = ai->platform().log();
        wabcut::ai::RequestLogEntry http;
        http.time = "2026-10-11T08:30:12Z";
        http.method = "POST";
        http.url = "https://api.anthropic.com/v1/messages";
        http.status = 200;
        log.add(http);
        wabcut::ai::RequestLogEntry call;
        call.kind = wabcut::ai::RequestLogEntry::Kind::Call;
        call.time = "2026-10-11T08:30:14Z";
        call.provider = "anthropic";
        call.model = "claude-sonnet-5-5";
        call.capability = "chat";
        call.cost = 2140;
        log.add(call);
        ai->platform().spend().record("anthropic", 3'420'000, wabcut::ai::todayUtc());

        // Open the page the way a user does (0.1.3): the Settings window's AI page (window.openAiPreferences: the
        // transcript's "set up transcription"; UX rework: the window hosts Settings since the left rail went).
        REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "openAiPreferences"));
        QObject* settings = shell.object("settingsWindowLoader")->property("item").value<QObject*>();
        REQUIRE(settings != nullptr);
        QObject* page = nullptr;
        // The pages are Loader delegates: walk the visual tree.
        std::function<QQuickItem*(QQuickItem*)> walk = [&](QQuickItem* item) -> QQuickItem* {
            if (!item || item->objectName() == QStringLiteral("aiPreferences"))
                return item;
            for (QQuickItem* child : item->childItems()) {
                if (QQuickItem* found = walk(child))
                    return found;
            }
            return nullptr;
        };
        REQUIRE(waitFor([&] { return (page = walk(settings->property("contentItem").value<QQuickItem*>())) != nullptr; }));
        INFO("Qt messages: " << join(takeMessages()));
        REQUIRE(page != nullptr);
        REQUIRE(waitFor([&] { return page->property("opened").toBool(); }));
        page->setProperty("currentTab", shot.tab);
        REQUIRE(shell.settle(true, kViolet));

        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        CHECK(image.size() == QSize(kShotWidth, kShotHeight));
        const QString path = outputDir() + QLatin1Char('/') + QLatin1String(shot.file);
        REQUIRE(image.save(path));
        UNSCOPED_INFO("wrote " << path.toStdString());
        const QStringList messages = takeMessages();
        INFO("Qt messages:\n" << join(messages));
        CHECK(messages.isEmpty());
    }
}

TEST_CASE("Consent prompt screenshot", "[ai][ui][screenshot]") {
    isolate();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    auto* ai = shell.engine.singletonInstance<AiController*>("Wabcut", "AiController");
    REQUIRE(ai != nullptr);

    QQmlComponent component(&shell.engine);
    component.setData("import QtQuick\nimport Wabcut\nAiConsentDialog {}", QUrl());
    QObject* dialog = component.createWithInitialProperties({{QStringLiteral("parent"), QVariant::fromValue(shell.window->contentItem())}});
    INFO(component.errorString().toStdString());
    REQUIRE(dialog != nullptr);
    const QVariantMap prompt = ai->consentPrompt(QStringLiteral("nova-3"), QStringLiteral("transcribe"),
                                                 QStringLiteral("Audio of 3 clips (12 min) from \"Interview A\""));
    REQUIRE(QMetaObject::invokeMethod(dialog, "ask", Q_ARG(QVariant, QVariant(prompt))));
    REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }));
    REQUIRE(shell.settle(true, kViolet));
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    REQUIRE(image.save(outputDir() + QStringLiteral("/ai-consent-prompt.png")));
    delete dialog;
    const QStringList messages = takeMessages();
    INFO("Qt messages:\n" << join(messages));
    CHECK(messages.isEmpty());
}

// Review wave 4, P2-3: the Local page's model list (a GPU probe and file stats) is not rebuilt on the GUI thread every
// second; the 1 s tick reports runtime changes only when there are any.
TEST_CASE("AiController keeps the local model list and status quiet while nothing changes", "[ai][ui]") {
    isolate();
    AiController ai;
    int models = 0;
    int status = 0;
    QObject::connect(&ai, &AiController::localModelsChanged, [&] { ++models; });
    QObject::connect(&ai, &AiController::localChanged, [&] { ++status; });
    REQUIRE_FALSE(ai.localModels().isEmpty());
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < 2600) { // two and a half ticks
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(10);
    }
    CHECK(models == 0);
    CHECK(status == 0);
    ai.refreshLocalModels(); // nothing changed: no rebuild of the delegates
    CHECK(models == 0);
}

// Review wave 4, P2-5: keys left in the plaintext file of the removed opt-in are offered for import. Wave 5, A5-P2-17:
// while the store keeps keys for this session only (no keychain, as here) the file stays until the user deletes it.
TEST_CASE("AiController moves legacy plaintext keys into the key store and keeps the file while they are session-only", "[ai][ui]") {
    isolate();
    AiController ai;
    const auto file = ai.legacyKeyFile();
    std::filesystem::create_directories(file.parent_path());
    {
        wabcut::ai::PlaintextFileSecretStore legacy(file);
        REQUIRE(legacy.write("anthropic", wabcut::ai::Secret(kSecret)));
    }
    CHECK(ai.legacyPlaintextKeys() == 1);
    const QString message = ai.importPlaintextKeys();
    INFO(message.toStdString());
    CHECK(message.contains(QStringLiteral("Moved 1 key")));
    CHECK(message.contains(QStringLiteral("kept")));
    CHECK(std::filesystem::exists(file)); // the keys live in memory only: the file still holds them for the next start
    CHECK(find(ai.providers(), QStringLiteral("id"), QStringLiteral("anthropic")).value(QStringLiteral("status")) != QStringLiteral("none"));
    CHECK_FALSE(allSettingsText().contains(QLatin1String(kSecret)));
    CHECK(ai.deletePlaintextKeys()); // the user's call, from Settings
    CHECK_FALSE(std::filesystem::exists(file));
    CHECK(ai.legacyPlaintextKeys() == 0);
}

// Review wave 4, P2-4: QML singletons have no destruction order. When AiController goes first, a generation worker
// that uses its platform is stopped and joined before the platform is destroyed.
TEST_CASE("AiController stops the generation worker before tearing the platform down", "[ai][ui]") {
    isolate();
    auto ai = std::make_unique<AiController>();
    wabcut::app::GenerateController generation;
    generation.setAiController(ai.get());
    std::atomic<bool> running{false};
    std::atomic<bool> finished{false};
    generation.runWorkerForTests([&, platform = &ai->platform()](std::stop_token stop) {
        running = true;
        while (!stop.stop_requested()) {
            (void)platform->providers().models(); // the platform is in use until the very end
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        finished = true;
    });
    REQUIRE(waitFor([&] { return running.load(); }, 5000));
    ai.reset(); // the engine destroying AiController first
    CHECK(finished.load()); // joined during ~AiController, before its platform was destroyed
}

// Review wave 5, A5-P2-19: the same for the bridges added since - translation (cloud chat, model downloads through the
// platform's transport), dub (the voice provider), rough cut, shorts, the audio and model-download bridges. A job that
// uses the platform when AiController goes is cancelled and joined first.
TEST_CASE("AiController stops the translate and dub jobs before tearing the platform down", "[ai][ui]") {
    isolate();
    auto ai = std::make_unique<AiController>();
    wabcut::app::TranslateBridge translate;
    wabcut::app::DubBridge dub;
    translate.stopJobsWith(ai.get());
    dub.stopJobsWith(ai.get());
    std::atomic<int> running{0};
    std::atomic<int> finished{0};
    const auto work = [&, platform = &ai->platform(), network = &ai->transport()](const std::atomic<bool>& cancelled)
        -> wabcut::app::BridgeJob::Finish {
        ++running;
        while (!cancelled.load()) {
            (void)platform->providers().models(); // the platform and its transport are in use until the very end
            (void)network;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        ++finished;
        return {};
    };
    REQUIRE(translate.runWorkerForTests(work));
    REQUIRE(dub.runWorkerForTests(work));
    REQUIRE(waitFor([&] { return running.load() == 2; }, 5000));
    ai.reset(); // the engine destroying AiController before the bridges
    CHECK(finished.load() == 2); // both joined during ~AiController
}

// Review wave 5, A5-P2-23 / S5-P2-9: the Dub dialog's consent said no project audio is sent, and the "speak" consent
// covered cloning, which uploads up to 90 s of a person's voice.
TEST_CASE("Dub consent names the recording when cloning and asks again after a speech grant", "[ai][ui][dub][privacy]") {
    isolate();
    Shell shell;
    auto* ai = shell.engine.singletonInstance<AiController*>("Wabcut", "AiController");
    auto* dub = shell.engine.singletonInstance<wabcut::app::DubBridge*>("Wabcut", "Dub");
    REQUIRE(ai != nullptr);
    REQUIRE(dub != nullptr);
    ai->saveKey(QStringLiteral("elevenlabs"), QString::fromLatin1(kSecret), /*skipCheck=*/true);
    ai->grantConsent(QStringLiteral("elevenlabs"), QStringLiteral("speak"));
    CHECK_FALSE(dub->consentPrompt(QStringLiteral("eleven_v3"), 120).value(QStringLiteral("needed")).toBool());
    const QVariantMap clone = dub->consentPrompt(QStringLiteral("eleven_v3"), 120, true);
    CHECK(clone.value(QStringLiteral("needed")).toBool());
    CHECK(clone.value(QStringLiteral("capability")).toString() == QStringLiteral("cloneVoice"));
    CHECK(clone.value(QStringLiteral("payload")).toString().contains(QStringLiteral("recording")));
    ai->grantConsent(QStringLiteral("elevenlabs"), QStringLiteral("cloneVoice"));
    CHECK_FALSE(dub->consentPrompt(QStringLiteral("eleven_v3"), 120, true).value(QStringLiteral("needed")).toBool());
}

// Review wave 5, A5-P2-27: "Place bullets with my AI" never asked for consent, though its tooltip said it would: the
// model call was refused and the cut quietly fell back to literal matching.
TEST_CASE("Rough cut placement with the cloud model asks for consent instead of quietly matching literally",
          "[ai][ui][roughcut][privacy]") {
    isolate();
    Shell shell;
    auto* ai = shell.engine.singletonInstance<AiController*>("Wabcut", "AiController");
    auto* roughCut = shell.engine.singletonInstance<wabcut::app::RoughCutBridge*>("Wabcut", "RoughCut");
    REQUIRE(ai != nullptr);
    REQUIRE(roughCut != nullptr);
    ai->saveKey(QStringLiteral("openai"), QString::fromLatin1(kSecret), /*skipCheck=*/true);
    ai->setChatModel(QStringLiteral("gpt-6.1-sol")); // a cloud chat model (the local tiers need no consent)
    REQUIRE(roughCut->cloudAvailable());
    QVariantMap prompt;
    const bool invoked = QMetaObject::invokeMethod(roughCut, "consentPrompt", Q_RETURN_ARG(QVariantMap, prompt), Q_ARG(int, 40));
    REQUIRE(invoked); // the sheet asks with it before Make the cut
    CHECK(prompt.value(QStringLiteral("needed")).toBool());
    CHECK(prompt.value(QStringLiteral("capability")).toString() == QStringLiteral("chat"));
    const QString refused = roughCut->preview(QStringLiteral("- the intro\n- why we started"), {{QStringLiteral("interpret"), true}});
    CHECK(refused.contains(QStringLiteral("consent")));
    CHECK_FALSE(roughCut->busy()); // nothing ran
}

// 0.2: the AI helpers' CPU cap is a preference (Preferences > AI > Models): persisted, applied to helpers started
// afterwards (the process-wide helper policy), 75 % by default.
TEST_CASE("AiController: the AI helpers' CPU limit is persisted and sets the helper policy (0.2)", "[ai][ui][cpu]") {
    isolate();
    const wabcut::ai::HelperCpuPolicy saved = wabcut::ai::helperCpuPolicy();
    {
        AiController ai;
        CHECK(ai.helperCpuLimit() == wabcut::ai::kDefaultHelperCpuPercent);
        ai.setHelperCpuLimit(50);
        CHECK(ai.helperCpuLimit() == 50);
        CHECK(wabcut::ai::helperCpuPolicy().cpuRatePercent == 50);
        CHECK(wabcut::ai::helperCpuPolicy().lowPriority);
        ai.setHelperCpuLimit(500); // clamped
        CHECK(ai.helperCpuLimit() == 100);
        ai.setHelperCpuLimit(25);
    }
    wabcut::ai::setHelperCpuPolicy({});
    {
        AiController again; // read back at start
        CHECK(again.helperCpuLimit() == 25);
        CHECK(wabcut::ai::helperCpuPolicy().cpuRatePercent == 25);
    }
    wabcut::ai::setHelperCpuPolicy(saved);
}

// 0.2: Generate lists the sound-effect model with its kind, and its estimate takes 0 seconds (the model picks the length,
// estimated at its longest) or a length up to 30 s.
TEST_CASE("Generate offers sound effects with their own lengths and price (0.2)", "[ai][ui][generation][sfx]") {
    isolate();
    AiController ai;
    wabcut::app::GenerateController generate;
    generate.setAiController(&ai);
    const QVariantMap sound = find(generate.models(), QStringLiteral("id"), QStringLiteral("eleven_text_to_sound_v2"));
    REQUIRE_FALSE(sound.isEmpty());
    CHECK(sound.value(QStringLiteral("sound")).toBool());
    CHECK(sound.value(QStringLiteral("loop")).toBool());
    CHECK(sound.value(QStringLiteral("maxSeconds")).toDouble() == 30.0);
    const QVariantMap four = generate.estimate(QStringLiteral("eleven_text_to_sound_v2"), QStringLiteral("rain on a tent"), 4, {}, false);
    CHECK(four.value(QStringLiteral("cost")).toString().contains(QStringLiteral("0.008")));
    CHECK(four.value(QStringLiteral("seconds")).toDouble() == 4.0);
    const QVariantMap picked = generate.estimate(QStringLiteral("eleven_text_to_sound_v2"), QStringLiteral("rain on a tent"), 0, {}, true);
    CHECK(picked.value(QStringLiteral("seconds")).toDouble() == 30.0);
    CHECK(picked.value(QStringLiteral("cost")).toString().contains(QStringLiteral("0.06")));
    CHECK_FALSE(generate.estimate(QStringLiteral("eleven_text_to_sound_v2"), QStringLiteral("rain"), 45, {}, false).value(QStringLiteral("ok")).toBool());
    // A video model keeps its own range.
    CHECK_FALSE(generate.estimate(QStringLiteral("gen4.5"), QStringLiteral("city"), 0, QStringLiteral("16:9")).value(QStringLiteral("ok")).toBool());
}
