// r07 local model library (Local-AI engineer): the LocalModels bridge over the AI network stack with a scripted
// transport (search, filters, a model's card, a download into the library index, ModelStore and AiController's registry,
// removal, offline mode, licence acceptance), the library models AiController brings back at start, the whisper model
// local transcription uses, and Settings > AI > Local models screenshots (dark and light, wide and narrow) rendered from
// the real Main.qml. Keys stay in memory (WABCUT_AI_KEYSTORE=memory) and models in a fresh WABCUT_CACHE_DIR; nothing
// reaches the network.

#include "ShellFixture.h"

#include "AiController.h"
#include "FakeTransport.h"
#include "LocalModelsController.h"
#include "TranscriptBridge.h"

#include <catch2/catch_test_macros.hpp>

#include <wabcut/ai/ModelLibrary.h>
#include <wabcut/ai/ModelStore.h>
#include <wabcut/ai/Sha256.h>
#include <wabcut/ai/llm/LocalRuntime.h>

#include <QCoreApplication>
#include <QSettings>
#include <QStandardPaths>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace wabcut::test;
using wabcut::app::LocalModelsController;
namespace ai = wabcut::ai;
namespace lib = wabcut::ai::library;

namespace {

const QColor kViolet(0x9B, 0x6C, 0xFF);
constexpr std::uint64_t GiB = std::uint64_t{1} << 30;
constexpr char kTinyRevision[] = "0123456789abcdef0123456789abcdef01234567";

std::filesystem::path freshCache(const char* name) {
    static std::atomic<int> counter{0};
    const auto dir = std::filesystem::path(outputDir().toStdU16String()) / "local-library" /
                     (std::string(name) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                      "-" + std::to_string(counter++));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    qputenv("WABCUT_CACHE_DIR", QString::fromStdU16String(dir.u16string()).toUtf8());
    return dir;
}

void isolate(const char* cache) {
    qputenv("WABCUT_AI_KEYSTORE", "memory");
    QStandardPaths::setTestModeEnabled(true);
    resetSettings();
    clearMessages();
    ai::ModelStore::setDefaultRoot({});
    freshCache(cache);
}

std::string fixture(const std::string& name) {
    std::ifstream in(std::filesystem::path(WABCUT_HF_FIXTURES) / name, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

ai::llm::SystemProbe probe(const char* os = "windows") {
    ai::llm::SystemProbe p;
    p.os = os;
    p.arch = "x64";
    p.cpuThreads = 16;
    p.ramTotal = 32 * GiB;
    p.ramAvailable = 24 * GiB;
    p.vulkanLoader = true;
    p.gpus.push_back({"NVIDIA GeForce RTX 3060", ai::llm::GpuVendor::Nvidia, 12 * GiB, 10 * GiB, false});
    return p;
}

/// A tiny GGUF repository: one Q4_K_M file of `body`, under `license`.
std::string tinyModel(const std::string& body, const std::string& license = "mit") {
    return R"({"id": "test/Tiny-GGUF", "author": "test", "sha": ")" + std::string(kTinyRevision) +
           R"(", "tags": ["gguf", "conversational", "license:)" + license + R"("], "pipeline_tag": "text-generation",
        "cardData": {"license": ")" + license + R"("}, "downloads": 12, "likes": 3,
        "siblings": [{"rfilename": "README.md", "size": 10},
                     {"rfilename": "Tiny-Q4_K_M.gguf", "size": )" + std::to_string(body.size()) +
           R"(, "lfs": {"sha256": ")" + ai::Sha256::hex(body) + R"(", "size": )" + std::to_string(body.size()) + "}}]}";
}

ai::test::ScriptedResponse rawBody(std::string body) {
    ai::test::ScriptedResponse r;
    r.status = 200;
    r.headers = {{"content-length", std::to_string(body.size())}};
    r.body = std::move(body);
    r.chunkSize = 512;
    return r;
}

bool hasRow(const QVariantList& rows, const QString& id) {
    return std::any_of(rows.begin(), rows.end(), [&](const QVariant& v) { return v.toMap().value(QStringLiteral("id")).toString() == id; });
}

/// Every item under `root` with this objectName (visual tree; Loader delegates included).
QQuickItem* findItem(QQuickItem* root, const QString& name) {
    if (!root)
        return nullptr;
    if (root->objectName() == name)
        return root;
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* found = findItem(child, name))
            return found;
    }
    return nullptr;
}

} // namespace

TEST_CASE("LocalModels searches filters opens and downloads a model through the AI network stack", "[ai][ui][library]") {
    isolate("controller");
    AiController aiController;
    LocalModelsController local;
    local.setAiController(&aiController);
    ai::test::FakeTransport net;
    local.setTransportForTesting(&net);
    local.setProbeForTesting(probe("linux")); // no llama-server runtime joins the download on this "computer"
    local.setFreeSpaceForTesting(100 * GiB);
    local.refresh();

    // Search: GGUF chat models only, the next page from the Link header.
    ai::test::ScriptedResponse search;
    search.body = fixture("search-qwen3-8b.json");
    search.headers = {{"content-type", "application/json"},
                      {"link", "<https://huggingface.co/api/models?filter=gguf&cursor=next1>; rel=\"next\""}};
    net.push(search);
    local.setQuery(QStringLiteral("Qwen3-8B"));
    local.search();
    REQUIRE(waitFor([&] { return local.searched() && !local.searching(); }));
    INFO(local.searchMessage().toStdString());
    CHECK(local.results().size() == 6);
    CHECK(local.hasMore());
    REQUIRE(net.requests().size() == 1);
    const ai::HttpRequest first = net.requests().front();
    CHECK(first.url.find("https://huggingface.co/api/models?filter=gguf&search=Qwen3-8B") == 0);
    CHECK_FALSE(ai::findHeader(first.headers, "authorization").has_value()); // no token saved: none sent

    local.setSortKey(QStringLiteral("benchmark")); // sorted here: no new request
    CHECK(net.requests().size() == 1);
    CHECK(local.results().front().toMap().value(QStringLiteral("score")).toDouble() > 0.0);
    local.setFilter(QStringLiteral("task"), QStringLiteral("vision"));
    CHECK(local.results().size() == 2);
    CHECK(local.hiddenCount() == 4);
    local.resetFilters();
    CHECK(local.results().size() == 6);

    // A model's card: quantizations, benchmarks with their source.
    net.pushJson(200, fixture("model-qwen3-8b-gguf.json"));
    local.openModel(QStringLiteral("Qwen/Qwen3-8B-GGUF"));
    REQUIRE(waitFor([&] { return !local.detail().value(QStringLiteral("loading")).toBool(); }));
    QVariantMap detail = local.detail();
    CHECK(detail.value(QStringLiteral("error")).toString().isEmpty());
    CHECK(detail.value(QStringLiteral("variants")).toList().size() == 5);
    CHECK(detail.value(QStringLiteral("defaultQuant")).toString() == QStringLiteral("Q4_K_M"));
    CHECK(detail.value(QStringLiteral("scoreSource")).toString().startsWith(QStringLiteral("https://")));
    CHECK_FALSE(detail.value(QStringLiteral("scores")).toList().isEmpty());

    // A tiny model: downloaded into the library, registered for routing and the local assistant, then removed.
    const std::string body(4096, 'g');
    net.pushJson(200, tinyModel(body));
    local.openModel(QStringLiteral("test/Tiny-GGUF"));
    REQUIRE(waitFor([&] { return !local.detail().value(QStringLiteral("loading")).toBool(); }));
    REQUIRE(local.detail().value(QStringLiteral("variants")).toList().size() == 1);
    std::atomic<int> finished{0};
    QObject::connect(&local, &LocalModelsController::downloadFinished, [&](const QString&, bool ok, const QString&) { finished += ok ? 1 : 100; });
    const QString id = QStringLiteral("hf:test/Tiny-GGUF:Q4_K_M");
    // The first attempt breaks off; "Try again" resumes it and the model still lands in the library.
    net.pushError(ai::AiErrorCode::Network, "connection reset");
    REQUIRE(local.download(QStringLiteral("test/Tiny-GGUF"), QStringLiteral("Q4_K_M")).isEmpty());
    REQUIRE(waitFor([&] { return finished.load() != 0; }));
    CHECK(finished == 100);
    REQUIRE(hasRow(local.downloads(), id));
    CHECK_FALSE(local.downloads().front().toMap().value(QStringLiteral("error")).toString().isEmpty());
    finished = 0;
    net.push(rawBody(body));
    REQUIRE(local.resume(id));
    REQUIRE(waitFor([&] { return finished.load() != 0; }));
    CHECK(finished == 1);
    const auto requests = net.requests();
    CHECK(requests.back().url == "https://huggingface.co/test/Tiny-GGUF/resolve/" + std::string(kTinyRevision) + "/Tiny-Q4_K_M.gguf");

    lib::LibraryIndex index(ai::ModelStore::defaultRoot());
    REQUIRE(index.load().has_value());
    REQUIRE(index.find(id.toStdString()) != nullptr);
    const ai::ModelInfo* registered = ai::findModel(id.toStdString());
    REQUIRE(registered != nullptr);
    CHECK(ai::ModelStore(ai::ModelStore::defaultRoot()).isInstalled(*registered));
    CHECK(hasRow(aiController.chatModels(), id));
    CHECK(hasRow(local.installed(), id));
    CHECK(QSettings().value(QStringLiteral("ai/userModels")).toStringList().filter(id).isEmpty());

    CHECK(local.remove(id).isEmpty());
    CHECK(ai::findModel(id.toStdString()) == nullptr);
    CHECK_FALSE(hasRow(aiController.chatModels(), id));
    CHECK_FALSE(hasRow(local.installed(), id));
    CHECK_FALSE(std::filesystem::exists(ai::ModelStore::defaultRoot() / "library" / "test" / "Tiny-GGUF" / "Tiny-Q4_K_M.gguf"));

    // Offline: no search, no download, and the page says why.
    aiController.setOfflineMode(true);
    CHECK(local.offline());
    const auto before = net.requests().size();
    local.search();
    CHECK_FALSE(local.searchMessage().isEmpty());
    CHECK_FALSE(local.download(QStringLiteral("test/Tiny-GGUF"), QStringLiteral("Q4_K_M")).isEmpty());
    CHECK(net.requests().size() == before);
}

TEST_CASE("LocalModels asks before a download under another licence", "[ai][ui][library]") {
    isolate("licence");
    AiController aiController;
    LocalModelsController local;
    local.setAiController(&aiController);
    ai::test::FakeTransport net;
    local.setTransportForTesting(&net);
    local.setProbeForTesting(probe("linux"));
    local.setFreeSpaceForTesting(100 * GiB);

    const std::string body(2048, 'l');
    net.pushJson(200, tinyModel(body, "llama3.1"));
    local.openModel(QStringLiteral("test/Tiny-GGUF"));
    REQUIRE(waitFor([&] { return !local.detail().value(QStringLiteral("loading")).toBool(); }));
    CHECK(local.detail().value(QStringLiteral("licenseNeedsAcceptance")).toBool());
    CHECK(local.detail().value(QStringLiteral("licenseName")).toString() == QStringLiteral("Llama 3.1 Community License"));
    const QString refused = local.download(QStringLiteral("test/Tiny-GGUF"), QStringLiteral("Q4_K_M"), false);
    CHECK(refused.contains(QStringLiteral("Llama 3.1 Community License")));
    CHECK(local.downloads().isEmpty());

    net.push(rawBody(body));
    REQUIRE(local.download(QStringLiteral("test/Tiny-GGUF"), QStringLiteral("Q4_K_M"), true).isEmpty());
    REQUIRE(waitFor([&] { return !local.downloading(); }));
    lib::LibraryIndex index(ai::ModelStore::defaultRoot());
    REQUIRE(index.load().has_value());
    const lib::InstalledModel* m = index.find("hf:test/Tiny-GGUF:Q4_K_M");
    REQUIRE(m != nullptr);
    CHECK(m->licenseAccepted);

    // Pause, resume and cancel of a queued download (the transport has nothing scripted: it would fail).
    CHECK(local.downloadWhisper(QStringLiteral("small")).isEmpty());
    REQUIRE(hasRow(local.downloads(), QStringLiteral("small")));
    CHECK(local.cancel(QStringLiteral("small")));
    REQUIRE(waitFor([&] { return !hasRow(local.downloads(), QStringLiteral("small")); }));
}

TEST_CASE("AiController brings library models back at start and keeps them out of the user models", "[ai][ui][library]") {
    isolate("restart");
    const std::string body(1024, 'r');
    {
        // What an earlier session downloaded.
        auto model = lib::parseHubModel(tinyModel(body));
        REQUIRE(model.has_value());
        const auto variants = lib::variantsOf(model->files);
        auto plan = lib::planInstall(*model, variants.front(), nullptr, false, lib::builtinBenchmarks());
        REQUIRE(plan.has_value());
        plan->context = 4096;
        ai::ModelStore store(ai::ModelStore::defaultRoot());
        const auto infos = lib::modelInfosOf(*plan);
        const auto path = store.pathFor(infos.front());
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << body;
        std::ofstream(path.string() + ".verified", std::ios::binary) << infos.front().sha256 << '\n';
        lib::LibraryIndex index(ai::ModelStore::defaultRoot());
        index.upsert(*plan);
        REQUIRE(index.save().has_value());
    }
    const QString id = QStringLiteral("hf:test/Tiny-GGUF:Q4_K_M");
    {
        AiController aiController;
        CHECK(hasRow(aiController.chatModels(), id));
        const ai::ModelInfo* info = ai::findModel(id.toStdString());
        REQUIRE(info != nullptr);
        CHECK(ai::llm::launchOverrides(id.toStdString()).context == 4096);
        aiController.setChatModel(id); // saves the settings
        CHECK(aiController.chatModel() == id);
        CHECK(QSettings().value(QStringLiteral("ai/userModels")).toStringList().filter(QStringLiteral("Tiny")).isEmpty());
        CHECK_FALSE(aiController.registerLocalModel(ai::ModelSpec{.id = "qwen3-8b", .provider = "local-llm"})); // built-in

        // QA-03: Use for assistant chooses the model at once, even when the engine cannot start yet (offline, no
        // llama-server downloaded): the assistant starts it on first use.
        aiController.setChatModel(QString());
        CHECK(aiController.defaultModel(QStringLiteral("chat")).isEmpty());
        aiController.setOfflineMode(true);
        LocalModelsController local;
        local.setAiController(&aiController);
        CHECK(local.useForAssistant(id).isEmpty());
        CHECK(aiController.defaultModel(QStringLiteral("chat")) == id);
        CHECK(aiController.chatModel() == id);
        aiController.setOfflineMode(false);
    }
    {
        // QA-03: the saved default names a downloaded model: it survives a restart (the library is loaded first).
        AiController again;
        CHECK(again.defaultModel(QStringLiteral("chat")) == id);
        CHECK(again.chatModel() == id);

        // QA-10: another storage folder: the models of the old one leave the lists; back again, they return.
        LocalModelsController local;
        local.setAiController(&again);
        const std::filesystem::path other = std::filesystem::temp_directory_path() /
                                            ("wabcut-r07-other-store-" + std::to_string(QCoreApplication::applicationPid()));
        std::error_code ec;
        std::filesystem::remove_all(other, ec);
        REQUIRE(local.setStorageFolder(QString::fromStdU16String(other.u16string())));
        CHECK_FALSE(hasRow(again.chatModels(), id));
        CHECK(again.libraryModelIds().isEmpty());
        CHECK(ai::findModel(id.toStdString()) == nullptr);
        CHECK(again.chatModel() != id);
        REQUIRE(local.setStorageFolder(QString()));
        CHECK(hasRow(again.chatModels(), id));
        CHECK(again.libraryModelIds().contains(id));
        std::filesystem::remove_all(other, ec);
    }
}

TEST_CASE("Local transcription follows the chosen whisper model", "[ai][ui][library][transcription]") {
    isolate("whisper");
    const QString automatic = LocalModelsController::transcriptionModelId();
    CHECK((automatic == QStringLiteral("base") || automatic == QStringLiteral("large-v3-turbo-q5_0")));
    LocalModelsController local;
    local.setWhisperModel(QStringLiteral("small"));
    CHECK(local.whisperModel() == QStringLiteral("small"));
    CHECK(LocalModelsController::transcriptionModelId() == QStringLiteral("small"));
    local.setWhisperModel(QStringLiteral("not-a-model")); // ignored
    CHECK(LocalModelsController::transcriptionModelId() == QStringLiteral("small"));
    {
        wabcut::app::TranscriptBridge t;
        const QVariantMap localRow = t.providers().front().toMap();
        CHECK(localRow.value(QStringLiteral("id")).toString() == QStringLiteral("local"));
        CHECK(localRow.value(QStringLiteral("detail")).toString().contains(QStringLiteral("small")));
    }
    CHECK(local.useForTranscription(QString()).isEmpty()); // back to automatic
    CHECK(local.whisperModel().isEmpty());
    CHECK(LocalModelsController::transcriptionModelId() == automatic);
}

TEST_CASE("Settings AI local models screenshots", "[ai][ui][screenshot][library]") {
    struct Look {
        const char* name;
        bool dark;
        int width;
        int height;
    };
    const Look looks[] = {{"dark-wide", true, kShotWidth, kShotHeight},
                          {"light-wide", false, kShotWidth, kShotHeight},
                          {"dark-narrow", true, 1100, 700},
                          {"light-narrow", false, 1100, 700}};
    for (const Look& look : looks) {
        INFO(look.name);
        isolate("shots");
        {
            // One downloaded library model so "Downloaded" has a row.
            const std::string body(512, 's');
            auto model = lib::parseHubModel(tinyModel(body));
            REQUIRE(model.has_value());
            auto plan = lib::planInstall(*model, lib::variantsOf(model->files).front(), nullptr, false, lib::builtinBenchmarks());
            REQUIRE(plan.has_value());
            plan->name = "Tiny test model";
            ai::ModelStore store(ai::ModelStore::defaultRoot());
            const auto infos = lib::modelInfosOf(*plan);
            const auto path = store.pathFor(infos.front());
            std::filesystem::create_directories(path.parent_path());
            std::ofstream(path, std::ios::binary) << body;
            std::ofstream(path.string() + ".verified", std::ios::binary) << infos.front().sha256 << '\n';
            lib::LibraryIndex index(ai::ModelStore::defaultRoot());
            index.upsert(*plan);
            REQUIRE(index.save().has_value());
        }
        Shell shell;
        REQUIRE(shell.window != nullptr);
        INFO(join(shell.errors));
        shell.resize(look.width, look.height);
        shell.setAppearance(look.dark, "ultraviolet");

        auto* local = shell.engine.singletonInstance<LocalModelsController*>("Wabcut", "LocalModels");
        REQUIRE(local != nullptr);
        ai::test::FakeTransport net;
        local->setTransportForTesting(&net);
        local->setProbeForTesting(probe());
        local->setFreeSpaceForTesting(412 * GiB);

        REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "openAiPreferences"));
        QObject* settings = shell.object("settingsWindowLoader")->property("item").value<QObject*>();
        REQUIRE(settings != nullptr);
        QQuickItem* content = settings->property("contentItem").value<QQuickItem*>();
        QQuickItem* aiPage = nullptr;
        REQUIRE(waitFor([&] { return (aiPage = findItem(content, QStringLiteral("aiPreferences"))) != nullptr; }));
        REQUIRE(waitFor([&] { return aiPage->property("opened").toBool(); }));
        spin(150); // openAiPreferences reveals the Transcription row once the window has opened (QA-12): let it land
        // The Local models tab, by its Settings search key (the tab order changed with the AI settings rework).
        bool shown = false;
        REQUIRE(QMetaObject::invokeMethod(aiPage, "reveal", Q_RETURN_ARG(bool, shown),
                                          Q_ARG(QVariant, QVariant(QStringLiteral("localAssistant")))));
        REQUIRE(shown);
        QQuickItem* page = nullptr;
        REQUIRE(waitFor([&] { return (page = findItem(content, QStringLiteral("localLlmPage"))) != nullptr; }));

        auto shoot = [&](const QString& name) {
            REQUIRE(shell.settle(look.dark, kViolet));
            const QImage image = shell.grab();
            REQUIRE_FALSE(image.isNull());
            const QString path = outputDir() + QStringLiteral("/preferences-ai-local-%1-%2.png").arg(name, QString::fromLatin1(look.name));
            REQUIRE(image.save(path));
            UNSCOPED_INFO("wrote " << path.toStdString());
        };

        REQUIRE(QMetaObject::invokeMethod(page, "showSection", Q_ARG(int, 0)));
        shoot(QStringLiteral("recommended"));

        ai::test::ScriptedResponse search;
        search.body = fixture("search-qwen3-8b.json");
        search.headers = {{"content-type", "application/json"}};
        net.push(search);
        REQUIRE(QMetaObject::invokeMethod(page, "showSection", Q_ARG(int, 1)));
        local->search();
        REQUIRE(waitFor([&] { return local->searched() && !local->searching(); }));
        REQUIRE(local->results().size() == 6);
        spin(200); // the cards lay out
        REQUIRE(QMetaObject::invokeMethod(page, "showSection", Q_ARG(int, 1)));
        shoot(QStringLiteral("browse"));
        // Further down: the result cards.
        if (QQuickItem* card = findItem(page, QStringLiteral("localModelCard_Qwen/Qwen3-8B-GGUF"))) {
            QQuickItem* flick = page->property("contentItem").value<QQuickItem*>();
            REQUIRE(flick != nullptr);
            QQuickItem* flickContent = flick->property("contentItem").value<QQuickItem*>();
            const qreal top = card->mapToItem(flickContent, QPointF(0, 0)).y();
            const qreal maxY = std::max<qreal>(0, flick->property("contentHeight").toReal() - flick->height());
            flick->setProperty("contentY", std::min(maxY, std::max<qreal>(0, top - 8)));
            shoot(QStringLiteral("browse-results"));
        } else {
            FAIL("no result card");
        }

        REQUIRE(QMetaObject::invokeMethod(page, "showSection", Q_ARG(int, 2)));
        shoot(QStringLiteral("transcription"));

        REQUIRE(QMetaObject::invokeMethod(page, "showSection", Q_ARG(int, 3)));
        shoot(QStringLiteral("downloaded"));

        // A model's card over the page.
        net.pushJson(200, fixture("model-qwen3-8b-gguf.json"));
        local->openModel(QStringLiteral("Qwen/Qwen3-8B-GGUF"));
        QObject* detail = page->findChild<QObject*>(QStringLiteral("localModelDetail"));
        REQUIRE(detail != nullptr);
        REQUIRE(QMetaObject::invokeMethod(detail, "show"));
        REQUIRE(waitFor([&] { return !local->detail().value(QStringLiteral("loading")).toBool() && detail->property("opened").toBool(); }));
        shoot(QStringLiteral("detail"));
        REQUIRE(QMetaObject::invokeMethod(detail, "close"));

        const QStringList messages = takeMessages();
        INFO("Qt messages:\n" << join(messages));
        CHECK(messages.isEmpty());
    }
}
