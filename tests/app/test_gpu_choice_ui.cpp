// Issue #10 (hybrid laptops), the app side: the preview's adapter goes to QT_D3D_ADAPTER_INDEX before any window exists
// (RhiPolicy's applyPreviewAdapter), the offscreen renderer follows the chosen GPU on this computer's real adapters, the
// Settings > Playback & performance > Graphics card row (Automatic or a listed GPU: exports at once, the preview after a
// restart; screenshots dark and light) and the Performance check's "Graphics card" row (the GPU in use and why;
// screenshots). The adapter lists are fakes of the issue's laptop (Radeon 780M as DXGI adapter 0, RTX 5060 Laptop GPU).

#include "ShellFixture.h"

#include "GpuBridge.h"
#include "PerformanceCheck.h"
#include "RhiPolicy.h"

#include <wabcut/editor/PerformanceProbe.h>
#include <wabcut/export/Encoders.h>
#include <wabcut/media/GpuInfo.h>
#include <wabcut/media/VideoDecoder.h>
#include <wabcut/render/OffscreenRenderer.h>

#include <catch2/catch_test_macros.hpp>

#include <QDateTime>
#include <QQuickItem>
#include <QSettings>

#include <algorithm>
#include <functional>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

const QString kRadeon = QStringLiteral("AMD Radeon(TM) 780M Graphics");
const QString kRtx = QStringLiteral("NVIDIA GeForce RTX 5060 Laptop GPU");

std::vector<media::GpuAdapter> hybridLaptop() {
    media::GpuAdapter radeon;
    radeon.name = kRadeon.toStdString();
    radeon.vendor = "AMD";
    radeon.vendorId = 0x1002;
    radeon.deviceId = 0x15BF;
    radeon.systemIndex = 0;
    radeon.highPerformanceIndex = 1;
    radeon.minimumPowerIndex = 0;
    radeon.kind = media::GpuKind::Integrated;
    radeon.dedicatedMemory = 512ull << 20;
    radeon.luid = 0x1000;
    media::GpuAdapter rtx;
    rtx.name = kRtx.toStdString();
    rtx.vendor = "NVIDIA";
    rtx.vendorId = 0x10DE;
    rtx.deviceId = 0x2D59;
    rtx.systemIndex = 1;
    rtx.highPerformanceIndex = 0;
    rtx.minimumPowerIndex = 1;
    rtx.kind = media::GpuKind::Discrete;
    rtx.dedicatedMemory = 8ull << 30;
    rtx.luid = 0x1001;
    return {radeon, rtx};
}

struct ResetGpus {
    ~ResetGpus() {
        media::setGpuChoice({});
        media::setGpuAdaptersForTesting(std::nullopt);
        qunsetenv("QT_D3D_ADAPTER_INDEX");
    }
};

QQuickItem* findNamed(QQuickItem* root, const QString& name) {
    if (!root)
        return nullptr;
    if (root->objectName() == name)
        return root;
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* found = findNamed(child, name))
            return found;
    }
    return nullptr;
}

QObject* settingsWindow(const Shell& shell) {
    QObject* loader = shell.object("settingsWindowLoader");
    return loader ? loader->property("item").value<QObject*>() : nullptr;
}

QObject* openSettings(Shell& shell, const QString& page) {
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "openSettings", Q_ARG(QString, page), Q_ARG(QString, QString())));
    QObject* window = settingsWindow(shell);
    REQUIRE(window != nullptr);
    REQUIRE(waitFor([&] { return window->property("opened").toBool(); }));
    return window;
}

QQuickItem* settingsPage(QObject* window, const QString& key) {
    QQuickItem* page = nullptr;
    REQUIRE(QMetaObject::invokeMethod(window, "page", Q_RETURN_ARG(QQuickItem*, page), Q_ARG(QString, key)));
    return page;
}

/// A menu's item by objectName (Menu.itemAt over its count: dynamic items are only in its content model).
QObject* menuItem(QObject* menu, const QString& name) {
    const int count = menu->property("count").toInt();
    for (int i = 0; i < count; ++i) {
        QQuickItem* item = nullptr;
        QMetaObject::invokeMethod(menu, "itemAt", Q_RETURN_ARG(QQuickItem*, item), Q_ARG(int, i));
        if (item && item->objectName() == name)
            return item;
    }
    return nullptr;
}

QObject* inDialog(QObject* dialog, const char* name) {
    const QString wanted = QString::fromLatin1(name);
    if (QObject* found = dialog->findChild<QObject*>(wanted))
        return found;
    return findNamed(dialog->property("contentItem").value<QQuickItem*>(), wanted);
}

QVariantMap rowOf(const PerformanceCheck& check, const QString& key) {
    for (const QVariant& row : check.rows()) {
        if (row.toMap().value(QStringLiteral("key")).toString() == key)
            return row.toMap();
    }
    return {};
}

/// A report as the check leaves it on the issue's laptop.
editor::PerformanceReport laptopReport() {
    editor::PerformanceReport r;
    r.width = 1920;
    r.height = 1080;
    r.frameRate = 30.0;
    r.cpuThreads = 16;
    for (const char* codec : {"H.264", "HEVC", "ProRes"}) {
        editor::DecodeMeasurement m;
        m.codec = codec;
        m.available = true;
        m.hardwareBackend = std::string(codec) == "ProRes" ? "" : "cuda";
        m.hardwareFps = std::string(codec) == "ProRes" ? 0.0 : 420.0;
        m.softwareFps = 160.0;
        r.decode.push_back(m);
    }
    r.encoders = {{"H.264", "h264_nvenc", true, ""}, {"HEVC", "hevc_nvenc", true, ""}};
    r.gpus = {"NVIDIA", "AMD"};
    r.graphics = "D3D11 · RTX 5060 Laptop GPU";
    r.gpuName = kRtx.toStdString();
    r.gpuReason = "highPerformance";
    r.gpuNames = {kRtx.toStdString(), kRadeon.toStdString()};
    r.audioDevice = true;
    r.audioName = "Speakers (Realtek(R) Audio)";
    r.seconds = 9.4;
    r.recommendation = editor::recommend(r);
    return r;
}

} // namespace

TEST_CASE("The preview's GPU goes to QT_D3D_ADAPTER_INDEX before any window unless one was set", "[app][gpu][rhi]") {
    ResetGpus reset;
    qunsetenv("QT_D3D_ADAPTER_INDEX");
    media::setGpuChoice({});
    media::setGpuAdaptersForTesting(media::GpuTestSetup{hybridLaptop()});
    // Automatic: the RTX, DXGI adapter 1 (adapter 0 is the integrated Radeon the display hangs on).
    CHECK(applyPreviewAdapter(RhiBackend::D3D11) == 1);
    CHECK(qgetenv("QT_D3D_ADAPTER_INDEX") == QByteArray("1"));
    CHECK(media::previewGpu().reason == media::GpuReason::HighPerformance);
    // D3D12 the same way; Vulkan, OpenGL and Metal pick their device themselves.
    qunsetenv("QT_D3D_ADAPTER_INDEX");
    media::setGpuAdaptersForTesting(media::GpuTestSetup{hybridLaptop()});
    CHECK(applyPreviewAdapter(RhiBackend::D3D12) == 1);
    qunsetenv("QT_D3D_ADAPTER_INDEX");
    media::setGpuAdaptersForTesting(media::GpuTestSetup{hybridLaptop()});
    CHECK(applyPreviewAdapter(RhiBackend::Vulkan) == -1);
    CHECK_FALSE(qEnvironmentVariableIsSet("QT_D3D_ADAPTER_INDEX"));
    // A developer's QT_D3D_ADAPTER_INDEX=0 before start is kept, and reported as the reason.
    media::setGpuAdaptersForTesting(media::GpuTestSetup{hybridLaptop(), media::WindowsGpuSetting::None, 0});
    qputenv("QT_D3D_ADAPTER_INDEX", "0");
    CHECK(applyPreviewAdapter(RhiBackend::D3D11) == -1);
    CHECK(qgetenv("QT_D3D_ADAPTER_INDEX") == QByteArray("0"));
    CHECK(media::previewGpu().reason == media::GpuReason::Override);
    CHECK(QString::fromStdString(media::previewGpu().adapter->name) == kRadeon);
    {
        // Fix round 1 (#9): the export encoders, the check and the Settings row follow the override too (even with
        // another card picked in Settings), so nothing claims to wait for a restart.
        const auto all = media::gpuAdapters();
        media::setGpuChoice(std::ranges::find(all, kRtx.toStdString(), &media::GpuAdapter::name)->key);
        GpuBridge bridge;
        CHECK(bridge.selectedName() == kRadeon);
        CHECK_FALSE(bridge.restartNeeded());
        CHECK(bridge.inUseReason() == QStringLiteral("set by QT_D3D_ADAPTER_INDEX"));
        CHECK(media::gpuVendors().front() == "AMD");
        media::setGpuChoice({});
    }
    // The integrated GPU picked in Settings: adapter 0.
    qunsetenv("QT_D3D_ADAPTER_INDEX");
    const auto listed = media::gpuAdapters();
    media::setGpuChoice(std::ranges::find(listed, kRadeon.toStdString(), &media::GpuAdapter::name)->key);
    media::setGpuAdaptersForTesting(media::GpuTestSetup{hybridLaptop()});
    CHECK(applyPreviewAdapter(RhiBackend::D3D11) == 0);
    CHECK(media::previewGpu().reason == media::GpuReason::UserChoice);
}

TEST_CASE("The offscreen renderer renders on the GPU Wabcut chose, on this computer's adapters", "[app][gpu][rhi]") {
#if !defined(_WIN32)
    SKIP("adapter choice is a Windows (DXGI) feature");
#else
    ResetGpus reset;
    qunsetenv("QT_D3D_ADAPTER_INDEX");
    media::setGpuAdaptersForTesting(std::nullopt);
    const std::vector<media::GpuAdapter> adapters = media::gpuAdapters();
    if (adapters.empty())
        SKIP("no hardware GPU");
    for (const media::GpuAdapter& a : adapters) {
        INFO("chosen: " << a.name << " (" << a.key << ")");
        media::setGpuChoice(a.key);
        media::setGpuAdaptersForTesting(std::nullopt); // a fresh preview decision, as at the next start
        REQUIRE(media::previewGpu().adapter->key == a.key);
        render::OffscreenOptions options;
        options.backend = render::RhiBackendRequest::D3D11;
        options.honourEnvironment = false;
        auto renderer = render::OffscreenRenderer::create(options);
        REQUIRE(renderer.has_value());
        CHECK((*renderer)->adapterName() == a.name);
    }
#endif
}

TEST_CASE("Graphics card setting: Automatic or one GPU - exports switch at once and the preview after a restart",
          "[app][gpu][settings][qml]") {
    resetSettings();
    clearMessages();
    ResetGpus reset;
    media::setGpuChoice({});
    media::setGpuAdaptersForTesting(media::GpuTestSetup{hybridLaptop()});
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* gpu = shell.engine.singletonInstance<GpuBridge*>("Wabcut", "Gpu");
    REQUIRE(gpu != nullptr);
    gpu->refresh();

    CHECK(gpu->choice().isEmpty());
    CHECK(gpu->inUseName() == kRtx);
    CHECK(gpu->inUseReason() == QStringLiteral("the high-performance GPU, chosen automatically"));
    CHECK(gpu->automaticText() == QStringLiteral("Automatic (%1)").arg(kRtx));
    CHECK_FALSE(gpu->restartNeeded());
    const QVariantList adapters = gpu->adapters();
    REQUIRE(adapters.size() == 2);
    const QVariantMap rtx = adapters.at(0).toMap();
    const QVariantMap radeon = adapters.at(1).toMap();
    CHECK(rtx.value(QStringLiteral("name")).toString() == kRtx);
    CHECK(rtx.value(QStringLiteral("kind")).toString() == QStringLiteral("discrete"));
    CHECK(rtx.value(QStringLiteral("detail")).toString() == QStringLiteral("Discrete · 8 GB"));
    CHECK(rtx.value(QStringLiteral("inUse")).toBool());
    CHECK(radeon.value(QStringLiteral("kind")).toString() == QStringLiteral("integrated"));
    CHECK(radeon.value(QStringLiteral("detail")).toString() == QStringLiteral("Integrated · saves battery"));
    const QString radeonKey = radeon.value(QStringLiteral("key")).toString();

    // Settings > Playback & performance > Graphics card.
    QObject* window = openSettings(shell, QStringLiteral("performance"));
    QQuickItem* page = nullptr;
    REQUIRE(waitFor([&] { return (page = settingsPage(window, QStringLiteral("performance"))) != nullptr; }));
    QQuickItem* row = findNamed(page, QStringLiteral("settingsGraphicsCard"));
    REQUIRE(row != nullptr);
    CHECK(row->property("description").toString().contains(QStringLiteral("Now: %1").arg(kRtx)));
    QQuickItem* button = findNamed(page, QStringLiteral("graphicsCardButton"));
    REQUIRE(button != nullptr);
    CHECK(button->property("text").toString() == QStringLiteral("Automatic"));
    auto* menu = button->findChild<QObject*>(QStringLiteral("graphicsCardMenu"));
    REQUIRE(menu != nullptr);
    REQUIRE(QMetaObject::invokeMethod(button, "clicked"));
    REQUIRE(waitFor([&] { return menu->property("opened").toBool(); }));
    REQUIRE(waitFor([&] { return menuItem(menu, QStringLiteral("graphicsCard_") + radeonKey) != nullptr; }));
    QObject* automatic = menuItem(menu, QStringLiteral("graphicsCardAutomatic"));
    REQUIRE(automatic != nullptr);
    CHECK(automatic->property("checked").toBool());
    spin(300);
    REQUIRE(shell.grab().save(outputDir() + QStringLiteral("/settings-graphics-card.png")));
    shell.setAppearance(false, "ultraviolet");
    REQUIRE(shell.settle(false, shell.themeColor("accentFill")));
    REQUIRE(shell.grab().save(outputDir() + QStringLiteral("/settings-graphics-card-light.png")));
    shell.setAppearance(true, "ultraviolet");

    // The integrated GPU: kept in the settings, the export encoders follow now, the preview at the next start.
    REQUIRE(QMetaObject::invokeMethod(menuItem(menu, QStringLiteral("graphicsCard_") + radeonKey), "triggered"));
    CHECK(gpu->choice() == radeonKey);
    CHECK(QSettings().value(QStringLiteral("graphics/adapter")).toString() == radeonKey);
    CHECK(media::gpuVendors().front() == "AMD");
#if defined(_WIN32)
    CHECK(exporter::hardwareEncoderOrder(exporter::VideoCodec::H264).front() == "h264_amf");
#endif
    CHECK(gpu->selectedName() == kRadeon);
    CHECK(gpu->inUseName() == kRtx); // this run's preview
    REQUIRE(waitFor([&] { return button->property("text").toString() == kRadeon; }));
    // Windows: the preview follows at the next start, and the row says so (elsewhere the system picks the preview's GPU).
    CHECK(gpu->restartNeeded() == gpu->previewFollows());
    if (gpu->previewFollows())
        CHECK(row->property("description").toString().contains(QStringLiteral("when Wabcut restarts")));
    REQUIRE(waitFor([&] { return !menu->property("visible").toBool(); }));

    // The next start applies the saved choice before anything asks.
    media::setGpuChoice({});
    GpuBridge::applySavedChoice();
    CHECK(QString::fromStdString(media::gpuChoice()) == radeonKey);
    media::setGpuAdaptersForTesting(media::GpuTestSetup{hybridLaptop()});
    CHECK(QString::fromStdString(media::previewGpu().adapter->name) == kRadeon);
    CHECK(media::previewGpu().reason == media::GpuReason::UserChoice);
    gpu->refresh();
    CHECK_FALSE(gpu->restartNeeded());
    CHECK(gpu->inUseReason() == QStringLiteral("chosen in Settings"));

    // Automatic again: the setting goes, the high-performance GPU for exports.
    gpu->setChoice(QString());
    CHECK_FALSE(QSettings().contains(QStringLiteral("graphics/adapter")));
    CHECK(media::gpuVendors().front() == "NVIDIA");
    CHECK(gpu->restartNeeded() == gpu->previewFollows()); // the preview took the Radeon at this "start"

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
    resetSettings();
}

TEST_CASE("Performance check: the Graphics card row says which GPU is used and why", "[app][gpu][performance][qml]") {
    resetSettings();
    clearMessages();
    ResetGpus reset;
    {
        QSettings settings;
        settings.setValue(QStringLiteral("performance/report"), QString::fromStdString(editor::toJson(laptopReport())));
        settings.setValue(QStringLiteral("performance/time"), QDateTime::currentDateTime().toString(Qt::ISODate));
        settings.setValue(QStringLiteral("performance/offered"), true);
    }
    {
        PerformanceCheck check(nullptr);
        REQUIRE(check.hasReport());
        CHECK(check.report()->gpuName == kRtx.toStdString());
        const QVariantMap row = rowOf(check, QStringLiteral("gpu"));
        REQUIRE_FALSE(row.isEmpty());
        CHECK(row.value(QStringLiteral("label")).toString() == QStringLiteral("Graphics card"));
        const QString value = row.value(QStringLiteral("value")).toString();
        CHECK(value.startsWith(kRtx + QStringLiteral(": the high-performance GPU, chosen automatically")));
        CHECK(value.contains(QStringLiteral("also here: %1").arg(kRadeon)));
        CHECK(row.value(QStringLiteral("good")).toBool());
    }
    // The reasons in words, and a choice still waiting for the restart.
    {
        editor::PerformanceReport r = laptopReport();
        r.gpuName = kRadeon.toStdString();
        r.gpuReason = "windowsPowerSaving";
        r.previewGpuName = kRtx.toStdString();
        QSettings().setValue(QStringLiteral("performance/report"), QString::fromStdString(editor::toJson(r)));
        PerformanceCheck check(nullptr);
        const QString value = rowOf(check, QStringLiteral("gpu")).value(QStringLiteral("value")).toString();
        CHECK(value.contains(QStringLiteral("Power saving in Windows Graphics settings")));
        CHECK(value.contains(QStringLiteral("the preview uses %1 until Wabcut restarts").arg(kRtx)));
        // The JSON keeps every field.
        CHECK(editor::reportFromJson(editor::toJson(r)) == r);
    }
    for (const char* key : {"noGpu", "onlyGpu", "highPerformance", "windowsPowerSaving", "windowsHighPerformance",
                            "windowsSpecific", "userChoice", "userChoiceMissing", "override"}) {
        INFO(key);
        CHECK_FALSE(GpuBridge::reasonText(QString::fromLatin1(key)).isEmpty());
    }
    QSettings().setValue(QStringLiteral("performance/report"), QString::fromStdString(editor::toJson(laptopReport())));

    // In the window: the dialog's summary has the row (screenshots, dark and light).
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* check = shell.engine.singletonInstance<PerformanceCheck*>("Wabcut", "PerformanceCheck");
    REQUIRE(check != nullptr);
    QObject* dialog = shell.object("firstRunCheckDialog");
    REQUIRE(dialog != nullptr);
    REQUIRE(QMetaObject::invokeMethod(check, "openCheck"));
    REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }));
    CHECK(dialog->property("stage").toString() == QStringLiteral("result"));
    REQUIRE(waitFor([&] { return inDialog(dialog, "performanceRow_gpu") != nullptr; }));
    spin(300);
    REQUIRE(shell.grab().save(outputDir() + QStringLiteral("/performance-check-gpu.png")));
    shell.setAppearance(false, "ultraviolet");
    REQUIRE(shell.settle(false, shell.themeColor("accentFill")));
    REQUIRE(shell.grab().save(outputDir() + QStringLiteral("/performance-check-gpu-light.png")));
    REQUIRE(QMetaObject::invokeMethod(dialog, "close"));
    REQUIRE(waitFor([&] { return !dialog->property("opened").toBool(); }));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
    resetSettings();
}
