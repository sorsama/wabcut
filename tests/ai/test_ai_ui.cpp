// AI platform UI (owner: AI platform engineer): the AiController bridge (display values only, keys never in
// settings, persistence) and Preferences > AI screenshots rendered offscreen from the real Main.qml through the
// Settings popover entry. Uses the UI shell fixture of tests/app (unchanged) and keeps keys in memory
// (WABCUT_AI_KEYSTORE=memory), so the developer's keychain is never touched.

#include "ShellFixture.h"

#include "AiController.h"
#include "GenerateController.h"
#include "ProjectController.h" // complete for CreatorBridge.h's inline setController
#include "AutopilotBridge.h"
#include "CreatorBridge.h"
#include "GlobalBridge.h"
#include "ShortsBridge.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include <wabcut/ai/ChildProcess.h>
#include <wabcut/ai/Secrets.h>
#include <wabcut/ai/llm/Tiers.h>

#include <QCoreApplication>
#include <QDate>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QThread>
#include <QFile>
#include <QMetaProperty>
#include <QQmlComponent>
#include <QQuickItem>
#include <QSet>
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
        // r07: the reviewed list lives in test_platform_registry ("Unverified registry entries are listed").
        CHECK(ai.unverifiedEntries().contains(QStringLiteral("model:voxtral-mini-latest")));
        CHECK(QDate::fromString(ai.registryUpdated(), Qt::ISODate).isValid());

        QVariantMap anthropic = find(ai.providers(), QStringLiteral("id"), QStringLiteral("anthropic"));
        REQUIRE_FALSE(anthropic.isEmpty());
        CHECK(anthropic.value(QStringLiteral("status")) == QStringLiteral("needsKey"));
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
        // r07: automatic routing; a local tier is only usable once downloaded, so with none here it may be "".
        INFO("chat model at start: " << ai.chatModel().toStdString());
        CHECK((ai.chatModel().isEmpty() || wabcut::ai::llm::tierForModel(ai.chatModel().toStdString()) != nullptr
               || ai.defaultModel(QStringLiteral("chat")).isEmpty()));
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

namespace {

/// Opens Settings > AI the way a user does (window.openAiPreferences: the transcript's "set up transcription"; the
/// Settings window hosts it) and returns the page once it shows.
QQuickItem* openAiPage(Shell& shell) {
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "openAiPreferences"));
    QObject* settings = shell.object("settingsWindowLoader")->property("item").value<QObject*>();
    REQUIRE(settings != nullptr);
    QQuickItem* page = nullptr;
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
    REQUIRE(waitFor([&] { return page->property("opened").toBool(); }));
    spin(150); // it reveals the Transcription row once the window has opened (QA-12): let that land before tabs change
    return page;
}

/// Every object named `name` under `root`: QObject children and, for items, the visual tree (delegates of views and
/// repeaters are visual children only), including the QObject children of those.
QList<QObject*> allNamed(QObject* root, const QString& name) {
    QList<QObject*> out;
    QSet<QObject*> seen;
    std::function<void(QObject*)> visit = [&](QObject* object) {
        if (!object || seen.contains(object))
            return;
        seen.insert(object);
        if (object->objectName() == name)
            out << object;
        for (QObject* child : object->children())
            visit(child);
        if (auto* item = qobject_cast<QQuickItem*>(object)) {
            for (QQuickItem* child : item->childItems())
                visit(child);
        }
    };
    visit(root);
    return out;
}

QObject* named(QObject* root, const char* name) {
    const QList<QObject*> found = allNamed(root, QLatin1String(name));
    return found.isEmpty() ? nullptr : found.front();
}

/// The loaded page of an AI tab ("aiFeaturesPage", "aiProvidersPage").
QObject* aiTab(QQuickItem* page, int tab, const char* name) {
    page->setProperty("currentTab", tab);
    QObject* found = nullptr;
    REQUIRE(waitFor([&] { return (found = named(page, name)) != nullptr; }));
    return found;
}

/// The model picker popover of one Features row.
QObject* pickerFor(QObject* features, const QString& capability) {
    for (QObject* picker : allNamed(features, QStringLiteral("aiModelPicker"))) {
        if (picker->property("capability").toString() == capability)
            return picker;
    }
    return nullptr;
}

QStringList idsOf(const QVariant& list) {
    QStringList out;
    for (const QVariant& v : list.toList())
        out << v.toMap().value(QStringLiteral("id")).toString();
    return out;
}

/// A stored key, a consent, caps and some request-log lines, so every page shows real rows.
void seedAi(AiController* ai) {
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
}

} // namespace

// r07: Settings > AI reworked (Features, Providers master-detail, Local models, Privacy, Usage). Each page is shot dark
// and light, at the default size and at the smallest window (1100 x 700, the Settings window then 1036 x 636), plus a
// screen smaller than that where the Providers panes stack (list, then the detail with Back). Nothing may clip.
TEST_CASE("Preferences > AI screenshots", "[ai][ui][screenshot]") {
    struct Shot {
        const char* file;
        int tab;
        bool dark;
        int width;
        int height;
        const char* provider; // Providers: the detail shown ("" = the first one)
    };
    const Shot shots[] = {
        {"preferences-ai-features.png", 0, true, kShotWidth, kShotHeight, ""},
        {"preferences-ai-features-light.png", 0, false, kShotWidth, kShotHeight, ""},
        {"preferences-ai-features-narrow.png", 0, true, 1100, 700, ""},
        {"preferences-ai-providers.png", 1, true, kShotWidth, kShotHeight, "anthropic"},
        {"preferences-ai-providers-light.png", 1, false, kShotWidth, kShotHeight, "anthropic"},
        {"preferences-ai-providers-narrow.png", 1, true, 1100, 700, "anthropic"},
        {"preferences-ai-providers-narrow-light.png", 1, false, 1100, 700, "ollama"},
        {"preferences-ai-providers-stacked.png", 1, true, 900, 640, ""},
        {"preferences-ai-providers-stacked-detail.png", 1, true, 900, 640, "anthropic"},
        {"preferences-ai-privacy.png", 3, true, kShotWidth, kShotHeight, ""},
        {"preferences-ai-privacy-narrow.png", 3, false, 1100, 700, ""},
        {"preferences-ai-usage.png", 4, true, kShotWidth, kShotHeight, ""},
        {"preferences-ai-usage-narrow.png", 4, false, 1100, 700, ""},
    };
    for (const Shot& shot : shots) {
        INFO(shot.file);
        isolate();
        Shell shell;
        REQUIRE(shell.window != nullptr);
        INFO(join(shell.errors));
        shell.resize(shot.width, shot.height);
        shell.setAppearance(shot.dark, "ultraviolet");

        auto* ai = shell.engine.singletonInstance<AiController*>("Wabcut", "AiController");
        REQUIRE(ai != nullptr);
        seedAi(ai);

        QQuickItem* page = openAiPage(shell);
        INFO("Qt messages: " << join(takeMessages()));
        page->setProperty("currentTab", shot.tab);
        if (shot.tab == 1) {
            aiTab(page, 1, "aiProvidersPage");
            if (*shot.provider != '\0')
                REQUIRE(QMetaObject::invokeMethod(page, "showProvider", Q_ARG(QString, QString::fromLatin1(shot.provider))));
        }
        REQUIRE(shell.settle(shot.dark, kViolet));

        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        CHECK(image.size() == QSize(shot.width, shot.height));
        const QString path = outputDir() + QLatin1Char('/') + QLatin1String(shot.file);
        REQUIRE(image.save(path));
        UNSCOPED_INFO("wrote " << path.toStdString());
        const QStringList messages = takeMessages();
        INFO("Qt messages:\n" << join(messages));
        CHECK(messages.isEmpty());
    }
}

TEST_CASE("Preferences > AI model picker and custom provider dialog screenshots", "[ai][ui][screenshot]") {
    for (const bool dark : {true, false}) {
        INFO("dark " << dark);
        // The assistant's model picker (Features).
        {
            isolate();
            Shell shell;
            REQUIRE(shell.window != nullptr);
            shell.resize(kShotWidth, kShotHeight);
            shell.setAppearance(dark, "ultraviolet");
            auto* ai = shell.engine.singletonInstance<AiController*>("Wabcut", "AiController");
            seedAi(ai);
            QQuickItem* page = openAiPage(shell);
            QObject* features = aiTab(page, 0, "aiFeaturesPage");
            spin(50);
            bool opened = false;
            REQUIRE(QMetaObject::invokeMethod(features, "openPicker", Q_RETURN_ARG(bool, opened), Q_ARG(QString, QStringLiteral("chat"))));
            REQUIRE(opened);
            QObject* picker = pickerFor(features, QStringLiteral("chat"));
            REQUIRE(picker != nullptr);
            REQUIRE(waitFor([&] { return picker->property("opened").toBool(); }));
            REQUIRE(shell.settle(dark, kViolet));
            const QImage image = shell.grab();
            REQUIRE(image.save(outputDir() + (dark ? QStringLiteral("/preferences-ai-model-picker.png")
                                                   : QStringLiteral("/preferences-ai-model-picker-light.png"))));
            const QStringList messages = takeMessages();
            INFO("Qt messages:\n" << join(messages));
            CHECK(messages.isEmpty());
        }
        // The custom provider dialog with every part filled and Advanced open, at the smallest window.
        {
            isolate();
            Shell shell;
            REQUIRE(shell.window != nullptr);
            shell.resize(dark ? kShotWidth : 1100, dark ? kShotHeight : 700);
            shell.setAppearance(dark, "ultraviolet");
            QQuickItem* page = openAiPage(shell);
            aiTab(page, 1, "aiProvidersPage");
            REQUIRE(QMetaObject::invokeMethod(page, "openCustomProvider", Q_ARG(QString, QString())));
            QObject* dialog = page->property("customProviderDialog").value<QObject*>();
            REQUIRE(dialog != nullptr);
            REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }));
            named(dialog, "aiCustomName")->setProperty("text", QStringLiteral("Studio gateway"));
            named(dialog, "aiCustomBaseUrl")->setProperty("text", QStringLiteral("https://llm.studio.example/v1"));
            dialog->setProperty("models", QVariantList{
                QVariantMap{{QStringLiteral("id"), QStringLiteral("studio-chat-large")}, {QStringLiteral("capability"), QStringLiteral("chat")}},
                QVariantMap{{QStringLiteral("id"), QStringLiteral("studio-whisper")}, {QStringLiteral("capability"), QStringLiteral("transcribe")}}});
            dialog->setProperty("advanced", true);
            dialog->setProperty("authStyle", QStringLiteral("header"));
            dialog->setProperty("headers", QVariantList{QVariantMap{{QStringLiteral("name"), QStringLiteral("x-team")},
                                                                    {QStringLiteral("value"), QStringLiteral("editors")}}});
            REQUIRE(shell.settle(dark, kViolet));
            const QImage image = shell.grab();
            REQUIRE(image.save(outputDir() + (dark ? QStringLiteral("/preferences-ai-custom-provider.png")
                                                   : QStringLiteral("/preferences-ai-custom-provider-narrow-light.png"))));
            const QStringList messages = takeMessages();
            INFO("Qt messages:\n" << join(messages));
            CHECK(messages.isEmpty());
        }
        // QA-08 / QA-01: Save checked the new key, nothing answered, so the form stays open with "Save without
        // checking"; the server's sound model is one of its rows.
        {
            isolate();
            Shell shell;
            REQUIRE(shell.window != nullptr);
            shell.resize(dark ? kShotWidth : 1100, dark ? kShotHeight : 700);
            shell.setAppearance(dark, "ultraviolet");
            QQuickItem* page = openAiPage(shell);
            aiTab(page, 1, "aiProvidersPage");
            REQUIRE(QMetaObject::invokeMethod(page, "openCustomProvider", Q_ARG(QString, QString())));
            QObject* dialog = page->property("customProviderDialog").value<QObject*>();
            REQUIRE(dialog != nullptr);
            REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }));
            named(dialog, "aiCustomName")->setProperty("text", QStringLiteral("Office server"));
            named(dialog, "aiCustomBaseUrl")->setProperty("text", QStringLiteral("http://127.0.0.1:9/v1"));
            named(dialog, "aiCustomKey")->setProperty("text", QStringLiteral("office-key-0123456789"));
            dialog->setProperty("models", QVariantList{
                QVariantMap{{QStringLiteral("id"), QStringLiteral("office-chat")}, {QStringLiteral("capability"), QStringLiteral("chat")}},
                QVariantMap{{QStringLiteral("id"), QStringLiteral("audioldm2")}, {QStringLiteral("capability"), QStringLiteral("sound")}}});
            REQUIRE(QMetaObject::invokeMethod(dialog, "save"));
            REQUIRE(waitFor([&] { return dialog->property("offerUnchecked").toBool(); }, 60'000));
            REQUIRE(shell.settle(dark, kViolet));
            const QImage image = shell.grab();
            REQUIRE(image.save(outputDir() + (dark ? QStringLiteral("/preferences-ai-custom-provider-check-failed.png")
                                                   : QStringLiteral("/preferences-ai-custom-provider-check-failed-narrow-light.png"))));
            const QStringList messages = takeMessages();
            INFO("Qt messages:\n" << join(messages));
            CHECK(messages.isEmpty());
        }
    }
}

TEST_CASE("Settings AI: the search keys open the right tab and Add a provider filters Providers", "[ai][ui][settings]") {
    isolate();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    auto* ai = shell.engine.singletonInstance<AiController*>("Wabcut", "AiController");
    REQUIRE(ai != nullptr);
    QQuickItem* page = openAiPage(shell);

    const std::pair<const char*, int> keys[] = {
        {"aiProviders", 1}, {"aiModels", 0}, {"aiPrivacy", 3}, {"aiUsage", 4}, {"localAssistant", 2},
        {"aiCustomProvider", 1}, {"aiDefaults", 0}, {"assistantModel", 0},
        // QA-12: a capability id shows Features (and that row).
        {"transcribe", 0}, {"translate", 0}, {"speak", 0}, {"image", 0}, {"video", 0}, {"sound", 0}, {"chat", 0},
    };
    for (const auto& [key, tab] : keys) {
        INFO(key);
        bool revealed = false;
        REQUIRE(QMetaObject::invokeMethod(page, "reveal", Q_RETURN_ARG(bool, revealed), Q_ARG(QVariant, QString::fromLatin1(key))));
        CHECK(revealed);
        CHECK(page->property("currentTab").toInt() == tab);
        spin(20);
    }
    bool revealed = true;
    REQUIRE(QMetaObject::invokeMethod(page, "reveal", Q_RETURN_ARG(bool, revealed), Q_ARG(QVariant, QStringLiteral("aiFeatures"))));
    CHECK(revealed); // the switch row flashes; the tab stays
    REQUIRE(QMetaObject::invokeMethod(page, "reveal", Q_RETURN_ARG(bool, revealed), Q_ARG(QVariant, QStringLiteral("nope"))));
    CHECK_FALSE(revealed);
    // QA-12: the transcript panel's "Set up" (window.openAiPreferences) shows Features, wherever the page was.
    page->setProperty("currentTab", 3);
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "openAiPreferences"));
    CHECK(waitFor([&] { return page->property("currentTab").toInt() == 0; }));

    // Features lists every capability; "Add a provider" on the transcription row shows Providers filtered by it.
    QObject* features = aiTab(page, 0, "aiFeaturesPage");
    for (const char* capability : {"chat", "transcribe", "translate", "speak", "image", "video", "sound"}) {
        INFO(capability);
        CHECK(waitFor([&] { return named(features, (std::string("aiFeatureRow_") + capability).c_str()) != nullptr; }));
    }
    QObject* add = named(features, "aiFeatureAddProvider_transcribe");
    REQUIRE(add != nullptr);
    REQUIRE(QMetaObject::invokeMethod(add, "clicked"));
    CHECK(page->property("currentTab").toInt() == 1);
    QObject* providers = aiTab(page, 1, "aiProvidersPage");
    CHECK(providers->property("filter").toString() == QStringLiteral("transcribe"));
    const QStringList transcribers = idsOf(providers->property("filtered"));
    CHECK(transcribers.contains(QStringLiteral("deepgram")));
    CHECK_FALSE(transcribers.contains(QStringLiteral("anthropic")));
    for (const QVariant& p : providers->property("filtered").toList())
        CHECK(p.toMap().value(QStringLiteral("caps")).toStringList().contains(QStringLiteral("transcribe")));

    // Search narrows the list; Connected shows a provider once it has a key.
    REQUIRE(QMetaObject::invokeMethod(providers, "showFilter", Q_ARG(QString, QStringLiteral("all"))));
    named(providers, "aiProviderSearch")->setProperty("text", QStringLiteral("deepg"));
    CHECK(idsOf(providers->property("filtered")) == QStringList{QStringLiteral("deepgram")});
    named(providers, "aiProviderSearch")->setProperty("text", QString());
    REQUIRE(QMetaObject::invokeMethod(providers, "showFilter", Q_ARG(QString, QStringLiteral("connected"))));
    CHECK_FALSE(idsOf(providers->property("filtered")).contains(QStringLiteral("anthropic")));
    ai->saveKey(QStringLiteral("anthropic"), QString::fromLatin1(kSecret), true);
    CHECK(waitFor([&] { return idsOf(providers->property("filtered")).contains(QStringLiteral("anthropic")); }));
    REQUIRE(QMetaObject::invokeMethod(page, "showProvider", Q_ARG(QString, QStringLiteral("anthropic"))));
    QObject* detail = named(providers, "aiProviderDetail");
    REQUIRE(detail != nullptr);
    CHECK(detail->property("providerId").toString() == QStringLiteral("anthropic"));
    CHECK(named(detail, "aiKeyField_anthropic") != nullptr);

    const QStringList messages = takeMessages();
    INFO("Qt messages:\n" << join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Settings AI Features: the picker sets a capability's model and Automatic clears it", "[ai][ui][settings]") {
    isolate();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    auto* ai = shell.engine.singletonInstance<AiController*>("Wabcut", "AiController");
    REQUIRE(ai != nullptr);
    ai->saveKey(QStringLiteral("deepgram"), QString::fromLatin1(kSecret), true);
    QQuickItem* page = openAiPage(shell);
    QObject* features = aiTab(page, 0, "aiFeaturesPage");
    REQUIRE(waitFor([&] { return pickerFor(features, QStringLiteral("transcribe")) != nullptr; }));

    QObject* picker = pickerFor(features, QStringLiteral("transcribe"));
    bool opened = false;
    REQUIRE(QMetaObject::invokeMethod(features, "openPicker", Q_RETURN_ARG(bool, opened), Q_ARG(QString, QStringLiteral("transcribe"))));
    REQUIRE(opened);
    REQUIRE(waitFor([&] { return picker->property("opened").toBool(); }));
    // The list offers Automatic first, then the models (Deepgram's among them).
    const QVariantList entries = picker->property("entries").toList();
    REQUIRE_FALSE(entries.isEmpty());
    CHECK(entries.front().toMap().value(QStringLiteral("kind")).toString() == QStringLiteral("auto"));
    CHECK(idsOf(entries).contains(QStringLiteral("nova-3")));

    REQUIRE(QMetaObject::invokeMethod(picker, "choose", Q_ARG(QString, QStringLiteral("nova-3"))));
    CHECK(ai->transcriptionModel() == QStringLiteral("nova-3"));
    CHECK(waitFor([&] { return !picker->property("visible").toBool(); }));
    QObject* status = named(features, "aiFeatureStatus_transcribe");
    REQUIRE(status != nullptr);
    CHECK(waitFor([&] { return status->property("text").toString().contains(QStringLiteral("Deepgram")); }));

    // Automatic: back to this computer.
    REQUIRE(QMetaObject::invokeMethod(picker, "choose", Q_ARG(QString, QString())));
    CHECK(ai->transcriptionModel().isEmpty());

    // Any model id by name, for the assistant: added to the provider and used.
    QObject* chat = pickerFor(features, QStringLiteral("chat"));
    REQUIRE(chat != nullptr);
    QObject* catalog = page->property("catalog").value<QObject*>();
    REQUIRE(catalog != nullptr);
    QString problem;
    REQUIRE(QMetaObject::invokeMethod(catalog, "useModelId", Q_RETURN_ARG(QString, problem), Q_ARG(QString, QStringLiteral("chat")),
                                      Q_ARG(QString, QStringLiteral("ollama")), Q_ARG(QString, QStringLiteral("gemma4:12b"))));
    CHECK(problem.isEmpty());
    CHECK(ai->chatModel() == QStringLiteral("gemma4:12b"));
    REQUIRE(QMetaObject::invokeMethod(catalog, "useModelId", Q_RETURN_ARG(QString, problem), Q_ARG(QString, QStringLiteral("chat")),
                                      Q_ARG(QString, QStringLiteral("ollama")), Q_ARG(QString, QStringLiteral("  "))));
    CHECK_FALSE(problem.isEmpty());
    CHECK(ai->removeModel(QStringLiteral("gemma4:12b")));

    const QStringList messages = takeMessages();
    INFO("Qt messages:\n" << join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Settings AI: the custom provider dialog validates and saves through AiController", "[ai][ui][settings][custom]") {
    isolate();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    auto* ai = shell.engine.singletonInstance<AiController*>("Wabcut", "AiController");
    REQUIRE(ai != nullptr);
    QQuickItem* page = openAiPage(shell);
    aiTab(page, 1, "aiProvidersPage");
    REQUIRE(QMetaObject::invokeMethod(page, "openCustomProvider", Q_ARG(QString, QString())));
    QObject* dialog = page->property("customProviderDialog").value<QObject*>();
    REQUIRE(dialog != nullptr);
    REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }));

    const auto problem = [&] {
        QString text;
        REQUIRE(QMetaObject::invokeMethod(dialog, "validate", Q_RETURN_ARG(QString, text)));
        return text;
    };
    CHECK(problem().contains(QStringLiteral("name")));
    named(dialog, "aiCustomName")->setProperty("text", QStringLiteral("Studio gateway"));
    named(dialog, "aiCustomBaseUrl")->setProperty("text", QStringLiteral("ftp://llm.example"));
    CHECK(problem().contains(QStringLiteral("http")));
    named(dialog, "aiCustomBaseUrl")->setProperty("text", QStringLiteral("http://127.0.0.1:9/v1")); // nothing answers there
    dialog->setProperty("authStyle", QStringLiteral("header"));
    CHECK(problem().isEmpty()); // an empty header name is the protocol's usual one
    named(dialog, "aiCustomAuthHeader")->setProperty("text", QStringLiteral("api key"));
    CHECK(problem().contains(QStringLiteral("header name")));
    named(dialog, "aiCustomAuthHeader")->setProperty("text", QStringLiteral("api-key"));
    dialog->setProperty("models", QVariantList{
        QVariantMap{{QStringLiteral("id"), QStringLiteral("m1")}, {QStringLiteral("capability"), QStringLiteral("chat")}},
        QVariantMap{{QStringLiteral("id"), QStringLiteral("m1")}, {QStringLiteral("capability"), QStringLiteral("chat")}}});
    CHECK(problem().contains(QStringLiteral("twice")));
    dialog->setProperty("models", QVariantList{
        QVariantMap{{QStringLiteral("id"), QStringLiteral("studio-chat")}, {QStringLiteral("capability"), QStringLiteral("chat")}}});
    dialog->setProperty("headers", QVariantList{QVariantMap{{QStringLiteral("name"), QStringLiteral("bad header")},
                                                            {QStringLiteral("value"), QStringLiteral("x")}}});
    CHECK(problem().contains(QStringLiteral("header name")));
    dialog->setProperty("headers", QVariantList{});
    dialog->setProperty("paths", QVariantMap{{QStringLiteral("chat"), QStringLiteral("v1/chat")}});
    CHECK(problem().contains(QStringLiteral("/")));
    dialog->setProperty("paths", QVariantMap{});
    CHECK(problem().isEmpty());

    QObject* catalog = page->property("catalog").value<QObject*>();
    REQUIRE(catalog != nullptr);

    // Save: the provider is listed (custom, local because it is on loopback), its model can be chosen, the key went to
    // the key store (never into settings), and Delete removes it again with its default.
    named(dialog, "aiCustomKey")->setProperty("text", QStringLiteral("studio-key-0123456789"));
    REQUIRE(QMetaObject::invokeMethod(dialog, "save"));
    INFO(dialog->property("error").toString().toStdString());
    CHECK(dialog->property("error").toString().isEmpty());
    // QA-08: Save checks a new key first; nothing answers at that address, so nothing is saved yet and the dialog offers
    // to save without checking.
    REQUIRE(waitFor([&] { return dialog->property("offerUnchecked").toBool(); }, 60'000));
    CHECK(dialog->property("visible").toBool());
    CHECK_FALSE(dialog->property("testOk").toBool());
    for (const QVariant& v : ai->providers())
        CHECK(v.toMap().value(QStringLiteral("name")).toString() != QStringLiteral("Studio gateway"));
    REQUIRE(QMetaObject::invokeMethod(dialog, "saveWithoutChecking"));
    CHECK(waitFor([&] { return !dialog->property("visible").toBool(); }));
    QVariantMap custom;
    for (const QVariant& v : ai->providers()) {
        if (v.toMap().value(QStringLiteral("name")).toString() == QStringLiteral("Studio gateway"))
            custom = v.toMap();
    }
    REQUIRE_FALSE(custom.isEmpty());
    const QString savedId = custom.value(QStringLiteral("id")).toString();
    CHECK(custom.value(QStringLiteral("custom")).toBool());
    CHECK(custom.value(QStringLiteral("hasKey")).toBool());
    CHECK_FALSE(allSettingsText().contains(QStringLiteral("studio-key-0123456789")));
    QObject* providers = named(page, "aiProvidersPage");
    CHECK(providers->property("selected").toString() == savedId); // the page shows what was saved

    // Edit reads it back without the key.
    REQUIRE(QMetaObject::invokeMethod(page, "openCustomProvider", Q_ARG(QString, savedId)));
    REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }));
    CHECK(named(dialog, "aiCustomName")->property("text").toString() == QStringLiteral("Studio gateway"));
    CHECK(named(dialog, "aiCustomAuthHeader")->property("text").toString() == QStringLiteral("api-key"));
    CHECK(named(dialog, "aiCustomKey")->property("text").toString().isEmpty());
    REQUIRE(QMetaObject::invokeMethod(dialog, "close"));

    bool used = false;
    QMetaObject::invokeMethod(catalog, "setDefault", Q_RETURN_ARG(bool, used), Q_ARG(QString, QStringLiteral("chat")),
                              Q_ARG(QString, QStringLiteral("studio-chat")));
    CHECK(used);
    CHECK(ai->chatModel() == QStringLiteral("studio-chat"));

    REQUIRE(QMetaObject::invokeMethod(page, "requestDelete", Q_ARG(QString, savedId)));
    QObject* confirm = named(page, "aiDeleteProviderConfirm");
    REQUIRE(confirm != nullptr);
    REQUIRE(QMetaObject::invokeMethod(confirm, "clicked"));
    CHECK(waitFor([&] { return idsOf(QVariant(ai->providers())).indexOf(savedId) < 0; }));
    CHECK(ai->chatModel() != QStringLiteral("studio-chat"));

    const QStringList messages = takeMessages();
    INFO("Qt messages:\n" << join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Settings AI: the custom provider dialog changes a saved provider only on Save", "[ai][ui][settings][custom]") {
    // QA-08: Test connection and Fetch models check the form as it is without saving it; Cancel leaves the saved
    // provider and its key as they were; Save checks a new key first and saves without checking only when asked.
    isolate();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    auto* ai = shell.engine.singletonInstance<AiController*>("Wabcut", "AiController");
    REQUIRE(ai != nullptr);
    const QVariantMap saved = ai->saveCustomProvider(QVariantMap{
        {QStringLiteral("name"), QStringLiteral("Office server")},
        {QStringLiteral("baseUrl"), QStringLiteral("http://127.0.0.1:9/v1")},
        {QStringLiteral("models"), QVariantList{QVariantMap{{QStringLiteral("id"), QStringLiteral("office-chat")}, {QStringLiteral("capability"), QStringLiteral("chat")}}}}});
    REQUIRE(saved.value(QStringLiteral("ok")).toBool());
    const QString id = saved.value(QStringLiteral("id")).toString();
    ai->saveKey(id, QStringLiteral("office-key-0123456789"), true);
    const auto masked = [&] { return find(ai->providers(), QStringLiteral("id"), id).value(QStringLiteral("masked")).toString(); };
    const auto unchanged = [&] {
        const QVariantMap spec = ai->customProvider(id);
        return spec.value(QStringLiteral("name")) == QStringLiteral("Office server") &&
               spec.value(QStringLiteral("baseUrl")) == QStringLiteral("http://127.0.0.1:9/v1") && masked().endsWith(QStringLiteral("6789"));
    };
    REQUIRE(unchanged());

    QQuickItem* page = openAiPage(shell);
    aiTab(page, 1, "aiProvidersPage");
    QObject* dialog = page->property("customProviderDialog").value<QObject*>();
    REQUIRE(dialog != nullptr);
    const auto edit = [&] {
        REQUIRE(QMetaObject::invokeMethod(page, "openCustomProvider", Q_ARG(QString, id)));
        REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }));
        named(dialog, "aiCustomName")->setProperty("text", QStringLiteral("Renamed server"));
        named(dialog, "aiCustomBaseUrl")->setProperty("text", QStringLiteral("http://127.0.0.1:9/v2"));
        named(dialog, "aiCustomKey")->setProperty("text", QStringLiteral("fresh-key-abcdefghij"));
    };

    // Test connection: the edited form is checked (nothing answers there), the saved provider stays as it was.
    edit();
    REQUIRE(QMetaObject::invokeMethod(dialog, "testConnection"));
    CHECK(dialog->property("testing").toBool());
    REQUIRE(waitFor([&] { return !dialog->property("testing").toBool(); }, 60'000));
    CHECK_FALSE(dialog->property("testOk").toBool());
    CHECK(unchanged());
    // Fetch models: the same.
    QObject* fetch = named(dialog, "aiCustomFetchModels");
    REQUIRE(fetch != nullptr);
    REQUIRE(QMetaObject::invokeMethod(fetch, "clicked"));
    REQUIRE(waitFor([&] { return dialog->property("listRequest").toString().isEmpty(); }, 60'000));
    CHECK(unchanged());
    // Cancel: nothing to undo.
    REQUIRE(QMetaObject::invokeMethod(dialog, "cancel"));
    REQUIRE(waitFor([&] { return !dialog->property("visible").toBool(); }));
    CHECK(unchanged());
    CHECK(find(ai->providers(), QStringLiteral("id"), id).value(QStringLiteral("name")) == QStringLiteral("Office server"));

    // Save with a new key: the key is checked first; it fails, so nothing changes until "Save without checking".
    edit();
    REQUIRE(QMetaObject::invokeMethod(dialog, "save"));
    REQUIRE(waitFor([&] { return dialog->property("offerUnchecked").toBool(); }, 60'000));
    CHECK(dialog->property("visible").toBool());
    CHECK(unchanged());
    QObject* unchecked = named(dialog, "aiCustomSaveUnchecked");
    REQUIRE(unchecked != nullptr);
    CHECK(unchecked->property("visible").toBool());
    REQUIRE(QMetaObject::invokeMethod(dialog, "saveWithoutChecking"));
    REQUIRE(waitFor([&] { return !dialog->property("visible").toBool(); }));
    CHECK(ai->customProvider(id).value(QStringLiteral("name")) == QStringLiteral("Renamed server"));
    CHECK(ai->customProvider(id).value(QStringLiteral("baseUrl")) == QStringLiteral("http://127.0.0.1:9/v2"));
    CHECK(masked().endsWith(QStringLiteral("ghij")));
    CHECK_FALSE(allSettingsText().contains(QStringLiteral("fresh-key-abcdefghij")));

    const QStringList messages = takeMessages();
    INFO("Qt messages:\n" << join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Offline mode and the no-cloud lock leave AI on this computer working in every feature", "[ai][ui][r07][privacy]") {
    // QA-06 / QA-07: offline mode and the project's no-cloud lock stop cloud models only. A custom provider on this
    // computer keeps the rough cut, shorts, autopilot, caption translation and the Generate panel working; a cloud
    // model does not.
    isolate();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    auto* ai = shell.engine.singletonInstance<AiController*>("Wabcut", "AiController");
    auto* rough = shell.engine.singletonInstance<wabcut::app::RoughCutBridge*>("Wabcut", "RoughCut");
    auto* shorts = shell.engine.singletonInstance<wabcut::app::ShortsBridge*>("Wabcut", "Shorts");
    auto* autopilot = shell.engine.singletonInstance<wabcut::app::AutopilotBridge*>("Wabcut", "Autopilot");
    auto* translate = shell.engine.singletonInstance<wabcut::app::TranslateBridge*>("Wabcut", "TranslateCaptions");
    auto* generation = shell.engine.singletonInstance<wabcut::app::GenerateController*>("Wabcut", "GenerateController");
    REQUIRE((ai && rough && shorts && autopilot && translate && generation));
    const QVariantMap saved = ai->saveCustomProvider(QVariantMap{
        {QStringLiteral("name"), QStringLiteral("Office server")},
        {QStringLiteral("baseUrl"), QStringLiteral("http://127.0.0.1:9/v1")},
        {QStringLiteral("auth"), QStringLiteral("none")},
        {QStringLiteral("models"), QVariantList{QVariantMap{{QStringLiteral("id"), QStringLiteral("office-chat")}, {QStringLiteral("capability"), QStringLiteral("chat")}},
                                                QVariantMap{{QStringLiteral("id"), QStringLiteral("office-image")}, {QStringLiteral("capability"), QStringLiteral("image")}}}}});
    REQUIRE(saved.value(QStringLiteral("ok")).toBool());
    REQUIRE(ai->setDefaultModel(QStringLiteral("chat"), QStringLiteral("office-chat")));
    REQUIRE(ai->setDefaultModel(QStringLiteral("image"), QStringLiteral("office-image")));
    ai->setOfflineMode(true);
    ai->setProjectLock(true);
    spin(50); // the consent ledger signals on the next turn
    CHECK(rough->cloudAvailable());
    CHECK(shorts->cloudAvailable());
    CHECK(autopilot->cloudAvailable());
    CHECK(translate->cloudAvailable());

    // The Generate panel prices the server's image model; a cloud model says why it cannot run.
    generation->open();
    QObject* panel = nullptr;
    REQUIRE(waitFor([&] { return (panel = shell.object("generatePanel")) != nullptr; }));
    const QVariantList models = generation->models();
    REQUIRE_FALSE(models.isEmpty());
    CHECK(models.front().toMap().value(QStringLiteral("id")) == QStringLiteral("office-image"));
    auto* prompt = shell.object("generationPrompt");
    REQUIRE(prompt != nullptr);
    prompt->setProperty("text", QStringLiteral("A lighthouse at dusk"));
    REQUIRE(waitFor([&] { return panel->property("quote").toMap().value(QStringLiteral("ok")).toBool(); }));

    // A cloud chat model with a key: offline and no-cloud stop it.
    ai->saveKey(QStringLiteral("anthropic"), QString::fromLatin1(kSecret), true);
    REQUIRE(ai->setDefaultModel(QStringLiteral("chat"), QStringLiteral("claude-sonnet-5-5")));
    CHECK_FALSE(rough->cloudAvailable());
    CHECK_FALSE(shorts->cloudAvailable());
    CHECK_FALSE(autopilot->cloudAvailable());
    CHECK_FALSE(translate->cloudAvailable());
    ai->setProjectLock(false);
    ai->setOfflineMode(false);
    spin(50);
    CHECK(rough->cloudAvailable());

    const QStringList messages = takeMessages();
    INFO("Qt messages:\n" << join(messages));
    CHECK(messages.isEmpty());
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
    CHECK(find(ai.providers(), QStringLiteral("id"), QStringLiteral("anthropic")).value(QStringLiteral("status")) != QStringLiteral("needsKey"));
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
