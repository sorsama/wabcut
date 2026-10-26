#pragma once

// RHI backend policy for the editor window (docs/architecture.md, "render": backend policy).
//
// Defaults: Direct3D 11 on Windows, Metal on macOS, Vulkan on Linux with a fallback to OpenGL when Vulkan has no
// usable device (an instance alone is not enough: the probe needs a physical device with a graphics queue).
// `--rhi <d3d11|d3d12|vulkan|opengl|metal>` overrides the default, and QSG_RHI_BACKEND (Qt's own variable) does
// the same with lower precedence. A backend that does not exist on the platform is ignored with a warning.
//
// At run time RhiFallbackHandler catches QQuickWindow::sceneGraphError (which Qt otherwise answers by terminating
// the application), switches to the next graphics API of rhiFallbackChain() and has the windows recreated.

#include <QByteArray>
#include <QObject>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>
#include <vector>

namespace wabcut::app {

enum class RhiBackend { D3D11, D3D12, Vulkan, OpenGL, Metal };

enum class RhiPlatform { Windows, MacOS, Linux };

// Where the resolved backend came from.
enum class RhiSource { PlatformDefault, Environment, CommandLine };

struct RhiChoice {
    RhiBackend backend = RhiBackend::D3D11;
    RhiSource source = RhiSource::PlatformDefault;
    QString warning; // non-empty when a request was ignored
};

// The platform this binary was built for.
RhiPlatform currentRhiPlatform();

// Parses "d3d11", "d3d12", "vulkan", "opengl"/"gl", "metal" (case-insensitive).
std::optional<RhiBackend> parseRhiBackend(QStringView name);

// Lower-case key as accepted by --rhi and QSG_RHI_BACKEND ("d3d11", ...).
QString rhiBackendKey(RhiBackend backend);

// Human-readable API name ("D3D11", "D3D12", "Vulkan", "OpenGL", "Metal").
QString rhiBackendDisplayName(RhiBackend backend);

bool isRhiBackendSupported(RhiBackend backend, RhiPlatform platform);
RhiBackend defaultRhiBackend(RhiPlatform platform);

// Pure resolution of the requested backend: `--rhi value` / `--rhi=value` in `arguments`, then the value of
// QSG_RHI_BACKEND (`environment`), then the platform default. Does not probe the GPU.
RhiChoice resolveRhiBackend(const QStringList& arguments, const QByteArray& environment, RhiPlatform platform);

QSGRendererInterface::GraphicsApi toGraphicsApi(RhiBackend backend);

// Whether Vulkan can render here: the loader creates an instance and it reports a physical device with a graphics
// queue (with the Vulkan headers at build time: QRhi::probe, which also creates a device).
bool probeVulkanDevice();

using VulkanProbe = bool (*)();

// Applies a choice before any QQuickWindow is created (requires a QGuiApplication). A Vulkan request is probed
// first (`probe`, injectable for tests) and falls back to the platform's non-Vulkan default when Vulkan has no
// usable device. Sets QSG_RHI_BACKEND to the final key so Qt and child processes agree, and returns the backend
// actually requested.
RhiBackend applyRhiBackend(const RhiChoice& choice, VulkanProbe probe = &probeVulkanDevice);

// Issue #10: puts the preview on media::previewGpu()'s adapter (Settings > Graphics card, else the Windows Graphics
// setting for Wabcut, else the high-performance GPU) before any window exists. D3D11 and D3D12 take it through
// QT_D3D_ADAPTER_INDEX, which every QRhi of the process reads (the offscreen renderer and the decoders' device follow the
// same adapter); a QT_D3D_ADAPTER_INDEX set before start is kept (previewGpu() reports it as the override). Vulkan,
// OpenGL and Metal pick their device themselves. Returns the index it set, or -1.
int applyPreviewAdapter(RhiBackend backend);

// "D3D11", "D3D12", "Vulkan", "OpenGL", "Metal", "Software" (or "Unknown").
QString graphicsApiDisplayName(QSGRendererInterface::GraphicsApi api);

// The graphics APIs to try, best first, after `failed` could not initialize the scene graph. Always ends with the
// software renderer, which needs no GPU.
std::vector<QSGRendererInterface::GraphicsApi> rhiFallbackChain(QSGRendererInterface::GraphicsApi failed,
                                                                RhiPlatform platform);

// Keeps the app alive when the scene graph cannot initialize (device lost at start-up, broken driver): connected to
// QQuickWindow::sceneGraphError, it moves to the next API of rhiFallbackChain() that was not tried yet. Another RHI
// backend applies to new windows, so the owner recreates its windows (main.cpp loads Main.qml again); the failing
// window is torn down after the signal returns. Qt picks the scene graph adaptation once per process, so the last
// resort, the software renderer, restarts the process with QT_QUICK_BACKEND=software. When every API has failed
// it calls `giveUp`. GUI thread.
class RhiFallbackHandler : public QObject {
    Q_OBJECT

public:
    struct Hooks {
        std::function<void()> recreateWindows;
        std::function<void()> restartWithSoftwareRenderer;
        std::function<void(const QString&)> giveUp;
    };

    // Starts from the current QQuickWindow::graphicsApi().
    explicit RhiFallbackHandler(Hooks hooks = {}, QObject* parent = nullptr);

    void setHooks(Hooks hooks) { m_hooks = std::move(hooks); }
    // Handles scene graph errors of `window`.
    void watch(QQuickWindow* window);
    QSGRendererInterface::GraphicsApi currentApi() const { return m_current; }

signals:
    void fellBack(const QString& from, const QString& to, const QString& message);

private:
    void handleError(QQuickWindow::SceneGraphError error, const QString& message);

    Hooks m_hooks;
    QSGRendererInterface::GraphicsApi m_current;
    std::vector<QSGRendererInterface::GraphicsApi> m_tried;
    RhiPlatform m_platform;
};

} // namespace wabcut::app
