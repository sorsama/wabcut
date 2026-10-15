// RHI backend policy (resolution, the Vulkan device probe, the run-time scene graph fallback), RendererInfo and the
// adapter-name shortening used by the GPU status pill.

#include "ShellFixture.h"

#include "RendererInfo.h"
#include "RhiPolicy.h"

#include <catch2/catch_test_macros.hpp>

#include <QQuickWindow>

#include <memory>
#include <vector>

using namespace wabcut::app;
using namespace wabcut::test;

TEST_CASE("RHI policy defaults per platform", "[app][rhi]") {
    CHECK(defaultRhiBackend(RhiPlatform::Windows) == RhiBackend::D3D11);
    CHECK(defaultRhiBackend(RhiPlatform::MacOS) == RhiBackend::Metal);
    CHECK(defaultRhiBackend(RhiPlatform::Linux) == RhiBackend::Vulkan);

    const RhiChoice choice = resolveRhiBackend({QStringLiteral("wabcut")}, {}, RhiPlatform::Windows);
    CHECK(choice.backend == RhiBackend::D3D11);
    CHECK(choice.source == RhiSource::PlatformDefault);
    CHECK(choice.warning.isEmpty());
}

TEST_CASE("RHI policy honours --rhi before QSG_RHI_BACKEND", "[app][rhi]") {
    const QStringList args{QStringLiteral("wabcut"), QStringLiteral("--rhi"), QStringLiteral("d3d12")};
    RhiChoice choice = resolveRhiBackend(args, "opengl", RhiPlatform::Windows);
    CHECK(choice.backend == RhiBackend::D3D12);
    CHECK(choice.source == RhiSource::CommandLine);

    choice = resolveRhiBackend({QStringLiteral("wabcut"), QStringLiteral("--rhi=Vulkan")}, {}, RhiPlatform::Windows);
    CHECK(choice.backend == RhiBackend::Vulkan);
    CHECK(choice.source == RhiSource::CommandLine);

    choice = resolveRhiBackend({QStringLiteral("wabcut")}, "opengl", RhiPlatform::MacOS);
    CHECK(choice.backend == RhiBackend::OpenGL);
    CHECK(choice.source == RhiSource::Environment);

    choice = resolveRhiBackend({QStringLiteral("wabcut")}, "gl", RhiPlatform::Linux);
    CHECK(choice.backend == RhiBackend::OpenGL);
}

TEST_CASE("RHI policy ignores unknown or unavailable backends with a warning", "[app][rhi]") {
    RhiChoice choice = resolveRhiBackend({QStringLiteral("--rhi"), QStringLiteral("metal")}, {}, RhiPlatform::Windows);
    CHECK(choice.backend == RhiBackend::D3D11);
    CHECK(choice.source == RhiSource::PlatformDefault);
    CHECK_FALSE(choice.warning.isEmpty());

    // An invalid --rhi falls through to a valid environment value.
    choice = resolveRhiBackend({QStringLiteral("--rhi"), QStringLiteral("glide")}, "d3d12", RhiPlatform::Windows);
    CHECK(choice.backend == RhiBackend::D3D12);
    CHECK(choice.source == RhiSource::Environment);
    CHECK_FALSE(choice.warning.isEmpty());

    choice = resolveRhiBackend({QStringLiteral("--rhi")}, "d3d11", RhiPlatform::Linux);
    CHECK(choice.backend == RhiBackend::Vulkan);
    CHECK_FALSE(choice.warning.isEmpty());
}

TEST_CASE("RHI backend keys round-trip", "[app][rhi]") {
    for (RhiBackend backend : {RhiBackend::D3D11, RhiBackend::D3D12, RhiBackend::Vulkan, RhiBackend::OpenGL, RhiBackend::Metal}) {
        const std::optional<RhiBackend> parsed = parseRhiBackend(rhiBackendKey(backend));
        REQUIRE(parsed.has_value());
        CHECK(*parsed == backend);
        CHECK_FALSE(rhiBackendDisplayName(backend).isEmpty());
        CHECK(toGraphicsApi(backend) != QSGRendererInterface::Unknown);
    }
    CHECK_FALSE(parseRhiBackend(u"software").has_value());
}

TEST_CASE("Adapter names are shortened for the GPU pill", "[app][rhi]") {
    CHECK(RendererInfo::shortAdapterName(QStringLiteral("NVIDIA GeForce RTX 3060")) == QStringLiteral("RTX 3060"));
    CHECK(RendererInfo::shortAdapterName(QStringLiteral("AMD Radeon RX 7800 XT")) == QStringLiteral("Radeon RX 7800 XT"));
    CHECK(RendererInfo::shortAdapterName(QStringLiteral("Intel(R) UHD Graphics 770")) == QStringLiteral("UHD Graphics 770"));
    CHECK(RendererInfo::shortAdapterName(QStringLiteral("Apple M2 Pro")) == QStringLiteral("Apple M2 Pro"));
    CHECK(RendererInfo::shortAdapterName(QStringLiteral("Microsoft Basic Render Driver"))
          == QStringLiteral("Microsoft Basic Render Driver"));
    CHECK(RendererInfo::shortAdapterName(QStringLiteral("NVIDIA")) == QStringLiteral("NVIDIA"));
    CHECK(RendererInfo::shortAdapterName(QString()).isEmpty());
}

namespace {

// Restores the process-wide graphics API and QSG_RHI_BACKEND that a test changes.
struct GraphicsApiGuard {
    QSGRendererInterface::GraphicsApi api = QQuickWindow::graphicsApi();
    QByteArray env = qgetenv("QSG_RHI_BACKEND");
    bool hadEnv = qEnvironmentVariableIsSet("QSG_RHI_BACKEND");
    ~GraphicsApiGuard() {
        QQuickWindow::setGraphicsApi(api);
        if (hadEnv)
            qputenv("QSG_RHI_BACKEND", env);
        else
            qunsetenv("QSG_RHI_BACKEND");
    }
};

bool noVulkanDevice() {
    return false;
}
bool vulkanDevice() {
    return true;
}

} // namespace

TEST_CASE("A Vulkan request without a usable device falls back before any window exists", "[app][rhi]") {
    GraphicsApiGuard guard;
    const RhiChoice vulkan{RhiBackend::Vulkan, RhiSource::CommandLine, {}};
    const RhiBackend fallback = defaultRhiBackend(currentRhiPlatform()) == RhiBackend::Vulkan
                                    ? RhiBackend::OpenGL
                                    : defaultRhiBackend(currentRhiPlatform());
    CHECK(applyRhiBackend(vulkan, &noVulkanDevice) == fallback);
    CHECK(QQuickWindow::graphicsApi() == toGraphicsApi(fallback));
    CHECK(qgetenv("QSG_RHI_BACKEND") == rhiBackendKey(fallback).toLatin1());
    CHECK(applyRhiBackend(vulkan, &vulkanDevice) == RhiBackend::Vulkan);
    CHECK(QQuickWindow::graphicsApi() == QSGRendererInterface::Vulkan);
    // The real probe answers without crashing whatever the machine has (no device, loader only, a GPU).
    const bool usable = probeVulkanDevice();
    INFO("Vulkan usable here: " << usable);
    SUCCEED();
}

TEST_CASE("The fallback chain ends with the software renderer", "[app][rhi]") {
    using Api = QSGRendererInterface::GraphicsApi;
    CHECK(rhiFallbackChain(Api::Direct3D11, RhiPlatform::Windows)
          == std::vector<Api>{Api::Direct3D12, Api::OpenGL, Api::Software});
    CHECK(rhiFallbackChain(Api::Vulkan, RhiPlatform::Linux) == std::vector<Api>{Api::OpenGL, Api::Software});
    CHECK(rhiFallbackChain(Api::Metal, RhiPlatform::MacOS) == std::vector<Api>{Api::OpenGL, Api::Software});
    CHECK(rhiFallbackChain(Api::OpenGL, RhiPlatform::Linux) == std::vector<Api>{Api::Vulkan, Api::Software});
    CHECK(rhiFallbackChain(Api::Software, RhiPlatform::Windows)
          == std::vector<Api>{Api::Direct3D11, Api::Direct3D12, Api::OpenGL});
    CHECK(graphicsApiDisplayName(Api::Software) == QStringLiteral("Software"));
}

TEST_CASE("A scene graph error falls back to the next graphics API instead of terminating", "[app][rhi]") {
    GraphicsApiGuard guard;
    // Start where the app starts on this platform.
    QQuickWindow::setGraphicsApi(toGraphicsApi(defaultRhiBackend(currentRhiPlatform())));

    std::vector<std::unique_ptr<QQuickWindow>> windows; // hidden, never shown
    QStringList fallbacks;
    QString gaveUp;
    int restarts = 0;
    RhiFallbackHandler handler;
    handler.setHooks({
        .recreateWindows =
            [&] {
                windows.push_back(std::make_unique<QQuickWindow>());
                handler.watch(windows.back().get());
            },
        .restartWithSoftwareRenderer = [&] { ++restarts; },
        .giveUp = [&](const QString& message) { gaveUp = message; },
    });
    QObject::connect(&handler, &RhiFallbackHandler::fellBack,
                     [&](const QString& from, const QString& to, const QString&) { fallbacks << from + "->" + to; });
    windows.push_back(std::make_unique<QQuickWindow>());
    handler.watch(windows.back().get());

    const std::vector<QSGRendererInterface::GraphicsApi> chain =
        rhiFallbackChain(handler.currentApi(), currentRhiPlatform());
    REQUIRE(chain.size() >= 2);
    REQUIRE(chain.back() == QSGRendererInterface::Software);
    clearMessages();
    // Inject one failure per GPU API: each moves to the next and recreates the window (the app keeps running).
    for (std::size_t i = 0; i + 1 < chain.size(); ++i) {
        const std::size_t before = windows.size();
        emit windows.back()->sceneGraphError(QQuickWindow::ContextNotAvailable, QStringLiteral("injected failure"));
        CHECK(handler.currentApi() == chain[i]);
        CHECK(QQuickWindow::graphicsApi() == chain[i]);
        REQUIRE(waitFor([&] { return windows.size() == before + 1; }));
    }
    CHECK(restarts == 0);
    // The last GPU API fails too: the process restarts on the software renderer.
    emit windows.back()->sceneGraphError(QQuickWindow::ContextNotAvailable, QStringLiteral("injected failure"));
    CHECK(handler.currentApi() == QSGRendererInterface::Software);
    REQUIRE(waitFor([&] { return restarts == 1; }));
    CHECK(fallbacks.size() == static_cast<qsizetype>(chain.size()));
    CHECK(fallbacks.last().endsWith(QStringLiteral("->Software")));
    CHECK(gaveUp.isEmpty());

    // When even that fails, the handler gives up (the app exits with an error instead of crashing).
    emit windows.back()->sceneGraphError(QQuickWindow::ContextNotAvailable, QStringLiteral("injected failure"));
    CHECK(gaveUp == QStringLiteral("injected failure"));
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.size() == static_cast<qsizetype>(chain.size()) + 1); // one warning per fallback, one critical
}

TEST_CASE("RendererInfo reads the backend on the scene graph's thread and survives teardown", "[app][rhi]") {
    QQuickWindow window;
    window.resize(64, 64);
    window.contentItem()->setSize(QSizeF(64, 64));
    auto info = std::make_unique<RendererInfo>();
    info->setWindow(&window);
    bool changed = false;
    QObject::connect(info.get(), &RendererInfo::infoChanged, [&] { changed = true; });
    // An offscreen grab initializes the scene graph on this (GUI) thread: the strings are applied at once.
    REQUIRE_FALSE(window.grabWindow().isNull());
    CHECK(info->ready());
    CHECK(info->api() == requestedRhiBackend());

    // A RendererInfo detached or destroyed before the next scene graph start receives nothing and does not crash.
    info->setWindow(nullptr);
    info.reset();
    REQUIRE_FALSE(window.grabWindow().isNull());

    // A new RendererInfo starts with the last capture of the process.
    RendererInfo later;
    CHECK(later.api() == requestedRhiBackend());
    (void)changed;
}
