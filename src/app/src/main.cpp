#include "AppShell.h"
#include "CrashHandler.h"
#include "CrashNotice.h"
#include "HomeBridge.h"
#include "RhiPolicy.h"
#include "SingleInstance.h"
#include "StartupProbe.h"
#include "SystemInfo.h"
#include "UiScale.h"
#include "WindowPlacement.h"

#include <wabcut/media/VideoDecoder.h>

#include <QCoreApplication>
#include <QGuiApplication>
#include <QIcon>
#include <QProcess>
#include <QProcessEnvironment>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QQuickStyle>
#include <QtQml/QQmlExtensionPlugin>

#include <functional>
#include <memory>
#include <string>

#if defined(WABCUT_WITH_AUDIO_PLUGINS)
#include <wabcut/plughost/PluginSystem.h>
#endif
#include <wabcut/platform/DllSearch.h>

// The QML module "Wabcut" is a static library (wabcut_ui); pull its plugin into the executable.
Q_IMPORT_QML_PLUGIN(WabcutPlugin)

int main(int argc, char* argv[]) {
    wabcut::platform::hardenDllSearch(); // first: no DLL from the current directory or PATH (S5-P2-5)
    // P.1: startup milestones for the benchmarks; inert unless WABCUT_PERF_STARTUP is set (docs/performance.md).
    wabcut::app::StartupProbe startupProbe;
    // QtCore Settings (Preferences.qml) needs all three identifiers. They are set before the application exists so
    // the saved interface size (M5j: 100 / 125 / 150 %) can become QT_SCALE_FACTOR, which Qt reads at construction.
    QCoreApplication::setApplicationName(QStringLiteral("Wabcut"));
    QCoreApplication::setOrganizationName(QStringLiteral("Wabcut"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("wabcut.org"));
    wabcut::app::applySavedUiScale();
    // Crash safety (I.7, docs/release.md): from here on a crash leaves a minidump and a redacted report in the app data
    // folder - nothing is uploaded - and the next start shows "Wabcut closed unexpectedly" (CrashNotice).
    {
        wabcut::crash::CrashOptions crashOptions;
        crashOptions.folder = wabcut::app::CrashNotice::defaultFolder();
#ifdef WABCUT_PRODUCT_VERSION
        crashOptions.version = WABCUT_PRODUCT_VERSION;
#else
        crashOptions.version = WABCUT_VERSION;
#endif
        if (wabcut::crash::install(crashOptions)) {
            for (const auto& [key, value] : wabcut::crash::systemDiagnostics()) {
                wabcut::crash::setDiagnostic(key, value);
            }
            std::string decoders;
            for (const std::string& backend : wabcut::media::hardwareBackendOrder()) {
                decoders += (decoders.empty() ? "" : ", ") + backend;
            }
            wabcut::crash::setDiagnostic("decode", decoders.empty() ? "software" : decoders + ", software");
            wabcut::crash::setDiagnostic("qt", qVersion());
            static QtMessageHandler previous = nullptr;
            previous = qInstallMessageHandler([](QtMsgType type, const QMessageLogContext& context, const QString& message) {
                wabcut::crash::log(qFormatLogMessage(type, context, message).toStdString()); // redacted inside
                if (previous) {
                    previous(type, context, message);
                }
            });
        }
    }
    startupProbe.mark("crashHandler");
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationVersion(QStringLiteral(WABCUT_VERSION));
    startupProbe.mark("application");
    // UX rework (stream S): one Wabcut opens the projects. A .wabcut double-clicked in File Explorer while Wabcut runs is
    // handed to that Wabcut, which opens it in its window, and this process ends here; otherwise this one listens for
    // later launches once its window is up (below). --new-instance starts a separate Wabcut; the startup benchmark always
    // gets its own.
    wabcut::app::SingleInstance instance(wabcut::app::SingleInstance::defaultServerName());
    const bool takesLaunches =
        !startupProbe.active() && !wabcut::app::SingleInstance::separateInstance(QCoreApplication::arguments());
    if (takesLaunches) {
        const QUrl project = wabcut::app::SingleInstance::projectToHandOver(QCoreApplication::arguments());
        if (!project.isEmpty() && instance.handOver({project})) {
            wabcut::crash::markCleanExit(); // this process only passed the file on
            return 0;
        }
    }
#if defined(WABCUT_WITH_AUDIO_PLUGINS)
    // A.6: the audio plugins - the catalog from its cache, new and changed plugin files scanned in the background (each
    // in its own wabcut-plughost, so a bad plugin only goes on the blocklist), plugin effects in the engine.
    wabcut::plughost::PluginSystem::start({.scanInBackground = true});
#endif
    startupProbe.mark("plugins");
    // The window and taskbar icon (design language v2: the W cut by one 12° slice).
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/qt/qml/Wabcut/resources/brand/wabcut-app-icon.svg")));

    // Graphics API before any window exists: D3D11 (Windows) / Metal (macOS) / Vulkan → OpenGL (Linux),
    // overridable with --rhi <d3d11|d3d12|vulkan|opengl|metal> or QSG_RHI_BACKEND. The GPU status pill shows
    // what the window actually got (RendererInfo).
    using namespace wabcut::app;
    const RhiChoice choice = resolveRhiBackend(QCoreApplication::arguments(), qgetenv("QSG_RHI_BACKEND"),
                                               currentRhiPlatform());
    const RhiBackend backend = applyRhiBackend(choice);
    qInfo("Wabcut: requesting the %s RHI backend", qPrintable(rhiBackendDisplayName(backend)));

    // Our own components draw every control; "Basic" is the lightest customizable style.
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    // The app opens on the Home screen (new project, recent projects, one-click import); the tests, which load the
    // same window, start on the editor.
    HomeBridge::setShowAtLaunch(true);

    // A scene graph that cannot initialize (broken driver, no usable device) would make Qt terminate the app; instead
    // the handler switches to the next graphics API and the window is loaded again by a fresh engine. This only
    // happens while the window starts up, before there is any work to lose.
    std::unique_ptr<QQmlApplicationEngine> engine;
    RhiFallbackHandler fallback;
    std::function<void()> start = [&] {
        engine = std::make_unique<QQmlApplicationEngine>();
        QObject::connect(
            engine.get(), &QQmlApplicationEngine::objectCreationFailed, &app, [] { QCoreApplication::exit(-1); },
            Qt::QueuedConnection);
        QObject::connect(engine.get(), &QQmlApplicationEngine::objectCreated, &fallback,
                         [&fallback](QObject* object, const QUrl&) {
                             if (auto* window = qobject_cast<QQuickWindow*>(object)) {
                                 fallback.watch(window);
                                 // UX rework: its place is kept for the next start (a child of the window).
                                 new WindowPlacementKeeper(window);
                             }
                         });
        // UX rework (stream S): the window opens where it was last time - size, position, maximized - fitted to the
        // screens there are now (initial properties: set before it shows, so it does not jump).
        engine->setInitialProperties(restoredWindowProperties());
        engine->loadFromModule("Wabcut", "Main");
        startupProbe.loaded(engine->rootObjects().isEmpty() ? nullptr
                                                            : qobject_cast<QQuickWindow*>(engine->rootObjects().constFirst()));
    };
    fallback.setHooks({
        .recreateWindows =
            [&] {
                engine.reset();
                start();
            },
        .restartWithSoftwareRenderer =
            [&instance] {
                // The scene graph adaptation is chosen once per process: start a new one on the software renderer. It
                // takes over the single-instance name (this one stops listening first, or the new one would hand its
                // project straight back here).
                instance.close();
                QProcess process;
                process.setProgram(QCoreApplication::applicationFilePath());
                process.setArguments(QCoreApplication::arguments().mid(1));
                QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
                env.insert(QStringLiteral("QT_QUICK_BACKEND"), QStringLiteral("software"));
                env.remove(QStringLiteral("QSG_RHI_BACKEND"));
                process.setProcessEnvironment(env);
                QCoreApplication::exit(process.startDetached() ? 0 : -1);
            },
        .giveUp = [](const QString&) { QCoreApplication::exit(-1); },
    });
    // A later launch handed its project over (SingleInstance): this window comes to the front and Main.qml opens it
    // through the normal path, with the unsaved-changes prompt (AppShell.openRequested).
    QObject::connect(&instance, &SingleInstance::filesRequested, &app, [&engine](const QList<QUrl>& files) {
        if (!engine || engine->rootObjects().isEmpty())
            return;
        bringToFront(qobject_cast<QWindow*>(engine->rootObjects().constFirst()));
        if (auto* shell = engine->singletonInstance<AppShell*>("Wabcut", "AppShell"))
            shell->requestOpen(files);
    });
    start();
    // Later launches are taken from here on, with the window loaded: before, this process could not answer them (its
    // event loop is not running yet), so they start on their own at once instead of waiting for it. A hand-over is only
    // taken while there is a window to open it in (else "no": that launch starts on its own, nothing is dropped). A
    // Wabcut already running keeps the name (listen() is false; this one is a separate Wabcut then). AppShell's
    // singleInstance (C++ diagnostics; QML does not read it) is known from here on.
    if (takesLaunches) {
        instance.setReady([&engine] { return engine && !engine->rootObjects().isEmpty(); });
        AppShell::setSingleInstance(instance.listen());
    }
    const int code = QGuiApplication::exec();
    wabcut::crash::markCleanExit(); // the next start shows no notice for this run
    return code;
}
