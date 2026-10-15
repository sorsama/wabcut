#include "RhiPolicy.h"

#include <QQuickWindow>
#include <QtGui/qtguiglobal.h>

#include <QLibrary>
#include <QMetaObject>

#include <algorithm>
#include <cstdint>

// QVulkanInstance is only declared when the Vulkan headers are present at build time (Vulkan SDK / distro
// package); without them the probe talks to the Vulkan loader library directly.
#if QT_CONFIG(vulkan) && __has_include(<vulkan/vulkan.h>)
#define WABCUT_HAVE_QVULKANINSTANCE 1
#include <QVulkanInstance>
#include <rhi/qrhi.h>
#endif

namespace wabcut::app {

RhiPlatform currentRhiPlatform() {
#if defined(Q_OS_WIN)
    return RhiPlatform::Windows;
#elif defined(Q_OS_MACOS)
    return RhiPlatform::MacOS;
#else
    return RhiPlatform::Linux;
#endif
}

std::optional<RhiBackend> parseRhiBackend(QStringView name) {
    const QString key = name.trimmed().toString().toLower();
    if (key == QLatin1String("d3d11"))
        return RhiBackend::D3D11;
    if (key == QLatin1String("d3d12"))
        return RhiBackend::D3D12;
    if (key == QLatin1String("vulkan"))
        return RhiBackend::Vulkan;
    if (key == QLatin1String("opengl") || key == QLatin1String("gl"))
        return RhiBackend::OpenGL;
    if (key == QLatin1String("metal"))
        return RhiBackend::Metal;
    return std::nullopt;
}

QString rhiBackendKey(RhiBackend backend) {
    switch (backend) {
    case RhiBackend::D3D11:
        return QStringLiteral("d3d11");
    case RhiBackend::D3D12:
        return QStringLiteral("d3d12");
    case RhiBackend::Vulkan:
        return QStringLiteral("vulkan");
    case RhiBackend::OpenGL:
        return QStringLiteral("opengl");
    case RhiBackend::Metal:
        return QStringLiteral("metal");
    }
    return {};
}

QString rhiBackendDisplayName(RhiBackend backend) {
    switch (backend) {
    case RhiBackend::D3D11:
        return QStringLiteral("D3D11");
    case RhiBackend::D3D12:
        return QStringLiteral("D3D12");
    case RhiBackend::Vulkan:
        return QStringLiteral("Vulkan");
    case RhiBackend::OpenGL:
        return QStringLiteral("OpenGL");
    case RhiBackend::Metal:
        return QStringLiteral("Metal");
    }
    return {};
}

bool isRhiBackendSupported(RhiBackend backend, RhiPlatform platform) {
    switch (backend) {
    case RhiBackend::D3D11:
    case RhiBackend::D3D12:
        return platform == RhiPlatform::Windows;
    case RhiBackend::Metal:
        return platform == RhiPlatform::MacOS;
    case RhiBackend::Vulkan: // macOS: through MoltenVK when Qt was built with it
    case RhiBackend::OpenGL:
        return true;
    }
    return false;
}

RhiBackend defaultRhiBackend(RhiPlatform platform) {
    switch (platform) {
    case RhiPlatform::Windows:
        return RhiBackend::D3D11;
    case RhiPlatform::MacOS:
        return RhiBackend::Metal;
    case RhiPlatform::Linux:
        return RhiBackend::Vulkan;
    }
    return RhiBackend::OpenGL;
}

namespace {

// Value of `--rhi value` or `--rhi=value`; the last occurrence wins. Empty when absent.
std::optional<QString> commandLineValue(const QStringList& arguments) {
    std::optional<QString> value;
    for (qsizetype i = 0; i < arguments.size(); ++i) {
        const QString& arg = arguments.at(i);
        if (arg == QLatin1String("--rhi")) {
            value = i + 1 < arguments.size() ? arguments.at(i + 1) : QString();
            ++i;
        } else if (arg.startsWith(QLatin1String("--rhi="))) {
            value = arg.mid(6);
        }
    }
    return value;
}

} // namespace

RhiChoice resolveRhiBackend(const QStringList& arguments, const QByteArray& environment, RhiPlatform platform) {
    RhiChoice choice{defaultRhiBackend(platform), RhiSource::PlatformDefault, {}};
    QStringList warnings;

    const auto consider = [&](const QString& requested, RhiSource source, const char* origin) {
        const std::optional<RhiBackend> backend = parseRhiBackend(requested);
        if (!backend) {
            warnings << QStringLiteral("%1: unknown RHI backend \"%2\" (expected d3d11, d3d12, vulkan, opengl or metal)")
                            .arg(QLatin1String(origin), requested);
            return false;
        }
        if (!isRhiBackendSupported(*backend, platform)) {
            warnings << QStringLiteral("%1: RHI backend \"%2\" is not available on this platform")
                            .arg(QLatin1String(origin), requested);
            return false;
        }
        choice.backend = *backend;
        choice.source = source;
        return true;
    };

    bool resolved = false;
    if (const std::optional<QString> value = commandLineValue(arguments))
        resolved = consider(*value, RhiSource::CommandLine, "--rhi");
    if (!resolved && !environment.trimmed().isEmpty())
        consider(QString::fromLocal8Bit(environment), RhiSource::Environment, "QSG_RHI_BACKEND");

    choice.warning = warnings.join(QStringLiteral("; "));
    return choice;
}

QSGRendererInterface::GraphicsApi toGraphicsApi(RhiBackend backend) {
    switch (backend) {
    case RhiBackend::D3D11:
        return QSGRendererInterface::Direct3D11;
    case RhiBackend::D3D12:
        return QSGRendererInterface::Direct3D12;
    case RhiBackend::Vulkan:
        return QSGRendererInterface::Vulkan;
    case RhiBackend::OpenGL:
        return QSGRendererInterface::OpenGL;
    case RhiBackend::Metal:
        return QSGRendererInterface::Metal;
    }
    return QSGRendererInterface::Unknown;
}

namespace {

#if !defined(WABCUT_HAVE_QVULKANINSTANCE) && QT_CONFIG(vulkan)
// The few Vulkan 1.0 declarations the loader probe needs (layouts and values from vulkan_core.h).
#if defined(_WIN32)
#define WABCUT_VKAPI __stdcall
#else
#define WABCUT_VKAPI
#endif
struct VkApplicationInfoT {
    std::uint32_t sType; // VK_STRUCTURE_TYPE_APPLICATION_INFO = 0
    const void* pNext;
    const char* pApplicationName;
    std::uint32_t applicationVersion;
    const char* pEngineName;
    std::uint32_t engineVersion;
    std::uint32_t apiVersion;
};
struct VkInstanceCreateInfoT {
    std::uint32_t sType; // VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO = 1
    const void* pNext;
    std::uint32_t flags;
    const VkApplicationInfoT* pApplicationInfo;
    std::uint32_t enabledLayerCount;
    const char* const* ppEnabledLayerNames;
    std::uint32_t enabledExtensionCount;
    const char* const* ppEnabledExtensionNames;
};
struct VkQueueFamilyPropertiesT {
    std::uint32_t queueFlags; // VK_QUEUE_GRAPHICS_BIT = 1
    std::uint32_t queueCount;
    std::uint32_t timestampValidBits;
    std::uint32_t minImageTransferGranularity[3];
};
using VkHandleT = void*; // VkInstance and VkPhysicalDevice are dispatchable (pointer) handles
using VkVoidFunctionT = void(WABCUT_VKAPI*)();
using GetInstanceProcAddrT = VkVoidFunctionT(WABCUT_VKAPI*)(VkHandleT, const char*);
using CreateInstanceT = std::int32_t(WABCUT_VKAPI*)(const VkInstanceCreateInfoT*, const void*, VkHandleT*);
using DestroyInstanceT = void(WABCUT_VKAPI*)(VkHandleT, const void*);
using EnumeratePhysicalDevicesT = std::int32_t(WABCUT_VKAPI*)(VkHandleT, std::uint32_t*, VkHandleT*);
using GetQueueFamilyPropertiesT = void(WABCUT_VKAPI*)(VkHandleT, std::uint32_t*, VkQueueFamilyPropertiesT*);
#endif

} // namespace

bool probeVulkanDevice() {
#if defined(WABCUT_HAVE_QVULKANINSTANCE)
    QVulkanInstance instance;
    if (!instance.create())
        return false;
    QRhiVulkanInitParams params;
    params.inst = &instance;
    return QRhi::probe(QRhi::Vulkan, &params); // picks a physical device and creates a VkDevice on it
#elif QT_CONFIG(vulkan)
#if defined(Q_OS_WIN)
    QLibrary loader(QStringLiteral("vulkan-1"));
#else
    QLibrary loader(QStringLiteral("vulkan"), 1);
#endif
    if (!loader.load())
        return false;
    const auto getProc = reinterpret_cast<GetInstanceProcAddrT>(loader.resolve("vkGetInstanceProcAddr"));
    if (!getProc)
        return false;
    const auto createInstance = reinterpret_cast<CreateInstanceT>(getProc(nullptr, "vkCreateInstance"));
    if (!createInstance)
        return false;
    const VkApplicationInfoT app{0, nullptr, "Wabcut", 1, "Wabcut", 1, 1u << 22}; // VK_API_VERSION_1_0
    const VkInstanceCreateInfoT info{1, nullptr, 0, &app, 0, nullptr, 0, nullptr};
    VkHandleT instance = nullptr;
    if (createInstance(&info, nullptr, &instance) != 0 || !instance)
        return false;
    const auto destroyInstance = reinterpret_cast<DestroyInstanceT>(getProc(instance, "vkDestroyInstance"));
    const auto enumerate = reinterpret_cast<EnumeratePhysicalDevicesT>(getProc(instance, "vkEnumeratePhysicalDevices"));
    const auto queueFamilies =
        reinterpret_cast<GetQueueFamilyPropertiesT>(getProc(instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
    bool usable = false;
    std::uint32_t deviceCount = 0;
    if (enumerate && queueFamilies && enumerate(instance, &deviceCount, nullptr) == 0 && deviceCount > 0) {
        std::vector<VkHandleT> devices(deviceCount);
        if (enumerate(instance, &deviceCount, devices.data()) >= 0) {
            for (std::uint32_t d = 0; d < deviceCount && !usable; ++d) {
                std::uint32_t familyCount = 0;
                queueFamilies(devices[d], &familyCount, nullptr);
                std::vector<VkQueueFamilyPropertiesT> families(familyCount);
                queueFamilies(devices[d], &familyCount, families.data());
                usable = std::ranges::any_of(families, [](const VkQueueFamilyPropertiesT& f) {
                    return (f.queueFlags & 1u) != 0 && f.queueCount > 0;
                });
            }
        }
    }
    if (destroyInstance)
        destroyInstance(instance, nullptr);
    return usable;
#else
    return false;
#endif
}

namespace {

RhiBackend nonVulkanDefault(RhiPlatform platform) {
    const RhiBackend fallback = defaultRhiBackend(platform);
    return fallback == RhiBackend::Vulkan ? RhiBackend::OpenGL : fallback;
}

} // namespace

RhiBackend applyRhiBackend(const RhiChoice& choice, VulkanProbe probe) {
    RhiBackend backend = choice.backend;
    if (!choice.warning.isEmpty())
        qWarning("Wabcut: %s", qPrintable(choice.warning));
    if (backend == RhiBackend::Vulkan && !(probe ? probe() : false)) {
        backend = nonVulkanDefault(currentRhiPlatform());
        qWarning("Wabcut: Vulkan has no usable device, falling back to %s",
                 qPrintable(rhiBackendDisplayName(backend)));
    }
    qputenv("QSG_RHI_BACKEND", rhiBackendKey(backend).toLatin1());
    QQuickWindow::setGraphicsApi(toGraphicsApi(backend));
    return backend;
}


// ---- Run-time fallback --------------------------------------------------------------------------------------------

QString graphicsApiDisplayName(QSGRendererInterface::GraphicsApi api) {
    switch (api) {
    case QSGRendererInterface::Direct3D11:
        return QStringLiteral("D3D11");
    case QSGRendererInterface::Direct3D12:
        return QStringLiteral("D3D12");
    case QSGRendererInterface::Vulkan:
        return QStringLiteral("Vulkan");
    case QSGRendererInterface::OpenGL:
        return QStringLiteral("OpenGL");
    case QSGRendererInterface::Metal:
        return QStringLiteral("Metal");
    case QSGRendererInterface::Software:
        return QStringLiteral("Software");
    default:
        return QStringLiteral("Unknown");
    }
}

std::vector<QSGRendererInterface::GraphicsApi> rhiFallbackChain(QSGRendererInterface::GraphicsApi failed,
                                                                RhiPlatform platform) {
    std::vector<RhiBackend> order;
    switch (platform) {
    case RhiPlatform::Windows:
        order = {RhiBackend::D3D11, RhiBackend::D3D12, RhiBackend::OpenGL};
        break;
    case RhiPlatform::MacOS:
        order = {RhiBackend::Metal, RhiBackend::OpenGL};
        break;
    case RhiPlatform::Linux:
        order = {RhiBackend::Vulkan, RhiBackend::OpenGL};
        break;
    }
    std::vector<QSGRendererInterface::GraphicsApi> chain;
    for (RhiBackend backend : order) {
        const auto api = toGraphicsApi(backend);
        if (api != failed && isRhiBackendSupported(backend, platform))
            chain.push_back(api);
    }
    if (failed != QSGRendererInterface::Software)
        chain.push_back(QSGRendererInterface::Software);
    return chain;
}

RhiFallbackHandler::RhiFallbackHandler(Hooks hooks, QObject* parent)
    : QObject(parent), m_hooks(std::move(hooks)), m_current(QQuickWindow::graphicsApi()),
      m_platform(currentRhiPlatform()) {}

void RhiFallbackHandler::watch(QQuickWindow* window) {
    // Connecting at all is what stops Qt from terminating on a scene graph error (it is emitted on the GUI thread).
    connect(window, &QQuickWindow::sceneGraphError, this, &RhiFallbackHandler::handleError, Qt::UniqueConnection);
}

void RhiFallbackHandler::handleError(QQuickWindow::SceneGraphError, const QString& message) {
    const QSGRendererInterface::GraphicsApi failed = m_current;
    if (std::ranges::find(m_tried, failed) == m_tried.end())
        m_tried.push_back(failed);
    QSGRendererInterface::GraphicsApi next = QSGRendererInterface::Unknown;
    for (QSGRendererInterface::GraphicsApi api : rhiFallbackChain(failed, m_platform)) {
        if (std::ranges::find(m_tried, api) == m_tried.end()) {
            next = api;
            break;
        }
    }
    if (next == QSGRendererInterface::Unknown) {
        qCritical("Wabcut: no graphics API could initialize the scene graph (last error: %s)", qPrintable(message));
        if (m_hooks.giveUp)
            m_hooks.giveUp(message);
        return;
    }
    m_current = next;
    const QString from = graphicsApiDisplayName(failed);
    const QString to = graphicsApiDisplayName(next);
    qWarning("Wabcut: the %s scene graph failed (%s); falling back to %s", qPrintable(from), qPrintable(message),
             qPrintable(to));
    emit fellBack(from, to, message);

    // Act after this signal returns: the failing window is still inside its emission.
    if (next == QSGRendererInterface::Software) {
        if (m_hooks.restartWithSoftwareRenderer)
            QMetaObject::invokeMethod(this, [this] { m_hooks.restartWithSoftwareRenderer(); }, Qt::QueuedConnection);
        return;
    }
    // New windows use the next RHI backend; QSG_RHI_BACKEND must agree with it.
    QQuickWindow::setGraphicsApi(next);
    for (RhiBackend backend : {RhiBackend::D3D11, RhiBackend::D3D12, RhiBackend::Vulkan, RhiBackend::OpenGL, RhiBackend::Metal}) {
        if (toGraphicsApi(backend) == next)
            qputenv("QSG_RHI_BACKEND", rhiBackendKey(backend).toLatin1());
    }
    if (m_hooks.recreateWindows)
        QMetaObject::invokeMethod(this, [this] { m_hooks.recreateWindows(); }, Qt::QueuedConnection);
}

} // namespace wabcut::app
