#include "AppShell.h"
#include "BackupSettings.h"
#include "CacheFolders.h"
#include "CrashHandler.h"
#include "CrashNotice.h"
#include "EnglishPlurals.h"
#include "FileOpenRelay.h"
#include "GpuBridge.h"
#include "HomeBridge.h"
#include "LaunchSplash.h"
#include "MediaReadinessBridge.h"
#include "RhiPolicy.h"
#include "SettingsBundle.h"
#include "SingleInstance.h"
#include "StartupProbe.h"
#include "SystemInfo.h"
#include "UiScale.h"
#include "UpdateBridge.h"
#include "WindowPlacement.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h> // GetKeyState, MessageBoxW: the Shift-at-launch question
#endif

#include <wabcut/media/GpuInfo.h>
#include <wabcut/media/VideoDecoder.h>

#include <QCoreApplication>
#include <QGuiApplication>
#include <QIcon>
#include <QPointer>
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
#if defined(WABCUT_WITH_OPENFX)
#include <wabcut/ofxhost/OfxSystem.h>
#endif
#endif
#include <wabcut/platform/DllSearch.h>

// The QML module "Wabcut" is a static library (wabcut_ui); pull its plugin into the executable.
Q_IMPORT_QML_PLUGIN(WabcutPlugin)

#if defined(_WIN32)
// Issue #10: hybrid laptops (NVIDIA Optimus, AMD PowerXpress / switchable graphics) start Wabcut on the discrete GPU
// without a Windows Graphics setting. The drivers look for these two exports in the executable that started the process
// - not in a DLL or a static library the linker could drop - so they live here, in wabcut.exe's own source. The test
// "wabcut.exe exports the hybrid GPU driver hints" reads them back from the executable's export table. The preview's
// adapter is also chosen explicitly below (applyPreviewAdapter): the hints alone may not reorder DXGI's adapters.
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

int main(int argc, char* argv[]) {
    wabcut::platform::hardenDllSearch(); // first: no DLL from the current directory or PATH (S5-P2-5)
    wabcut::media::useSystemVaapiDrivers(); // Linux: hardware decoding with the distribution's VA-API drivers
    // P.1: startup milestones for the benchmarks; inert unless WABCUT_PERF_STARTUP is set (docs/performance.md).
    wabcut::app::StartupProbe startupProbe;
    // QtCore Settings (Preferences.qml) needs all three identifiers. They are set before the application exists so
    // the saved interface size (M5j: 100 / 125 / 150 %) can become QT_SCALE_FACTOR, which Qt reads at construction.
    QCoreApplication::setApplicationName(QStringLiteral("Wabcut"));
    QCoreApplication::setOrganizationName(QStringLiteral("Wabcut"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("wabcut.org"));
    // 0.7 (card 028): a settings import or reset that was staged and not applied (the restart that should have applied it
    // did not happen) is applied now, before any setting is read.
    wabcut::app::SettingsBundle().applyStaged();
#if defined(_WIN32)
    // 0.7 (card 028): Shift held while Wabcut starts offers the default settings (Premiere: hold Alt). Asked before the
    // application exists, because the interface size is read right below. The current settings are backed up first and
    // the recent projects stay. Not for the startup benchmark or the tests.
    if (!startupProbe.active() && (GetKeyState(VK_SHIFT) & 0x8000) != 0 &&
        MessageBoxW(nullptr,
                    L"Start Wabcut with its default settings?\n\nYour current settings are saved to a backup file first, "
                    L"and your recent projects stay as they are.",
                    L"Wabcut", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2 | MB_TOPMOST | MB_SETFOREGROUND) == IDYES) {
        wabcut::app::SettingsBundle bundle;
        if (bundle.stageReset(true).value(QStringLiteral("ok")).toBool())
            bundle.applyStaged();
    }
#endif
    wabcut::app::applySavedUiScale();
    // 0.7 (card 029): a new install keeps autosaves and backups in a "Wabcut Auto-Save" folder next to each project; an
    // install from before that stays "beside the project" until the user changes it. Decided once, before anything is saved.
    wabcut::app::backup::chooseDefaultForNewInstall();
    // P1 #48: Settings > Media & cache > AI models and analysis folder, before any AI service asks where its cache is.
    wabcut::app::cachefolders::applyChosenAiCacheFolder();
    // Issue #10: Settings > Graphics card before anything asks which GPU to use (the decoders' order below already does).
    wabcut::app::GpuBridge::applySavedChoice();
    // Crash safety (I.7, docs/release.md): from here on a crash leaves a minidump and a redacted report in the app data
    // folder - nothing is uploaded - and the next start shows "Wabcut closed unexpectedly" (CrashNotice).
    {
        wabcut::crash::CrashOptions crashOptions;
        crashOptions.folder = wabcut::app::CrashNotice::defaultFolder();
#ifdef WABCUT_PRODUCT_VERSION
        crashOptions.version = WABCUT_PRODUCT_VERSION;
        wabcut::app::CrashNotice::setProductVersion(QStringLiteral(WABCUT_PRODUCT_VERSION)); // Help > Report a Bug…
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
            if (const wabcut::media::GpuSelection gpu = wabcut::media::previewGpu(); gpu.adapter) {
                wabcut::crash::setDiagnostic("gpu", gpu.adapter->name + " (" + std::string(wabcut::media::gpuReasonKey(gpu.reason)) + ")");
            }
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
#if defined(Q_OS_LINUX)
    // X11 first, through XWayland on a Wayland desktop: the launch intro and the window placement position their windows,
    // which Wayland does not let an application do. The Wayland platform plugin (shipped too) where there is no X server.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "xcb;wayland");
#endif
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationVersion(QStringLiteral(WABCUT_VERSION));
    // Real English plurals for the "%n clip(s)" strings, before any QML loads (EnglishPlurals.h).
    wabcut::app::EnglishPluralTranslator englishPlurals;
    QCoreApplication::installTranslator(&englishPlurals);
    // Background rendering while idle (MediaReadiness, editor/IdleRender.h) runs in the application only.
    wabcut::app::MediaReadinessBridge::setBackgroundRenderAvailable(true);
    startupProbe.mark("application");
    // UX rework (stream S): one Wabcut opens the projects. A .wabcut double-clicked in File Explorer while Wabcut runs is
    // handed to that Wabcut, which opens it in its window, and this process ends here; otherwise this one listens for
    // later launches once its window is up (below). --new-instance starts a separate Wabcut; the startup benchmark always
    // gets its own.
    wabcut::app::SingleInstance instance(wabcut::app::SingleInstance::defaultServerName());
    const bool takesLaunches =
        !startupProbe.active() && !wabcut::app::SingleInstance::separateInstance(QCoreApplication::arguments());
    if (takesLaunches) {
        // 0.7 (card 005): every file the launch names - a recording or timeline file opened with Wabcut in File Explorer
        // as well as a project - goes to the running Wabcut, which imports it (or opens the project).
        const QList<QUrl> files = wabcut::app::SingleInstance::filesToHandOver(QCoreApplication::arguments());
        // Review: File Explorer starts one process per selected file (a verb without multi-select) - with Wabcut closed
        // they must not each start a Wabcut. The first to get the launch lock starts it; the others wait for it to listen
        // (a few seconds at most) and hand their file to it. A launch without a file claims the lock too, so a Wabcut
        // that is starting is not raced by the files launched right after it.
        if (!files.isEmpty()) {
            if (instance.handOverOrWait(files)) {
                wabcut::crash::markCleanExit(); // this process only passed the file on
                return 0;
            }
        } else {
            instance.claimLaunch();
        }
    }
#if defined(WABCUT_WITH_AUDIO_PLUGINS)
    // A.6: the audio plugins - the catalog from its cache, new and changed plugin files scanned in the background (each
    // in its own wabcut-plughost, so a bad plugin only goes on the blocklist), plugin effects in the engine.
    wabcut::plughost::PluginSystem::start({.scanInBackground = true});
#endif
#if defined(WABCUT_WITH_OPENFX)
    // 0.7: the OpenFX video plug-ins - the catalog from its cache, new and changed plug-in binaries scanned in the background
    // (each in its own wabcut-ofxhost), and their effects in the model and the playback layer.
    wabcut::ofxhost::OfxSystem::start({.scanInBackground = true});
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
    // Issue #10: the preview on the GPU decode and export use (QT_D3D_ADAPTER_INDEX; one set before start is kept).
    const int previewAdapter = applyPreviewAdapter(backend);

    // Our own components draw every control; "Basic" is the lightest customizable style.
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    // The app opens on the Home screen (new project, recent projects, one-click import); the tests, which load the
    // same window, start on the editor.
    HomeBridge::setShowAtLaunch(true);

    // Design language v3: the echo mark assembles on the desktop first, in a see-through window of its own
    // (LaunchSplash); the main window loads under it once the mark is complete, and the mark fades out above it when the
    // window has drawn. Not with the intro turned off or reduce motion (Settings > Appearance), for the startup
    // benchmark (it measures the window), or in the process restarted on the software renderer.
    QPointer<LaunchSplash> splash;
    if (!startupProbe.active() && qEnvironmentVariable("QT_QUICK_BACKEND") != QLatin1String("software") &&
        LaunchSplash::wanted()) {
        const QVariantMap placed = startWindowProperties(QStringLiteral("window"), kWindowMinimum, kWindowDefaultSize);
        const QPoint centre = placed.contains(QStringLiteral("width"))
                                  ? QRect(placed.value(QStringLiteral("x")).toInt(), placed.value(QStringLiteral("y")).toInt(),
                                          placed.value(QStringLiteral("width")).toInt(),
                                          placed.value(QStringLiteral("height")).toInt())
                                        .center()
                                  : QPoint();
        splash = new LaunchSplash(centre, &app);
        QObject::connect(splash, &LaunchSplash::finished, splash, &QObject::deleteLater);
        if (!splash->show())
            delete splash.data();
    }

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
        // screens there are now (initial properties: set before it shows, so it does not jump). P1 #47: never larger
        // than the work area, even with the 1100 x 700 minimum; a first run on a small screen opens maximized.
        engine->setInitialProperties(startWindowProperties(QStringLiteral("window"), kWindowMinimum, kWindowDefaultSize));
        engine->loadFromModule("Wabcut", "Main");
        // Opt-in update checks (Settings > General > Updates; off by default): the daily one, if it is on.
        if (auto* updates = engine->singletonInstance<UpdateBridge*>("Wabcut", "Updates"))
            updates->startAutomaticChecks();
        QQuickWindow* window =
            engine->rootObjects().isEmpty() ? nullptr : qobject_cast<QQuickWindow*>(engine->rootObjects().constFirst());
        startupProbe.loaded(window);
        if (splash)
            splash->dismissOnFirstFrame(window);
    };
    fallback.setHooks({
        .recreateWindows =
            [&] {
                engine.reset();
                start();
            },
        .restartWithSoftwareRenderer =
            [&instance, previewAdapter] {
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
                if (previewAdapter >= 0) // ours, not the user's: the new process decides again
                    env.remove(QStringLiteral("QT_D3D_ADAPTER_INDEX"));
                process.setProcessEnvironment(env);
                QCoreApplication::exit(process.startDetached() ? 0 : -1);
            },
        .giveUp = [](const QString&) { QCoreApplication::exit(-1); },
    });
    // A later launch handed its project over (SingleInstance): this window comes to the front and Main.qml opens it
    // through the normal path, with the unsaved-changes prompt (AppShell.openRequested).
    const auto openInWindow = [&engine](const QList<QUrl>& files) {
        if (!engine || engine->rootObjects().isEmpty())
            return;
        bringToFront(qobject_cast<QWindow*>(engine->rootObjects().constFirst()));
        if (auto* shell = engine->singletonInstance<AppShell*>("Wabcut", "AppShell"))
            shell->requestOpen(files);
    };
    QObject::connect(&instance, &SingleInstance::filesRequested, &app, openInWindow);
    // macOS (FileOpenRelay.h): Finder's double-click, Open With and Dock drops arrive as an event, also before the window
    // exists; the files wait until `launch` has loaded it.
    FileOpenRelay fileOpen;
    app.installEventFilter(&fileOpen);
    QObject::connect(&fileOpen, &FileOpenRelay::filesRequested, &app, openInWindow);
    const auto launch = [&] {
        start();
        fileOpen.setReady(engine && !engine->rootObjects().isEmpty());
        // Later launches are taken from here on, with the window loaded: before, this process could not answer them
        // (its event loop is not running yet, or the window is not there), so they start on their own at once instead
        // of waiting for it. A hand-over is only taken while there is a window to open it in (else "no": that launch
        // starts on its own, nothing is dropped). A Wabcut already running keeps the name (listen() is false; this one
        // is a separate Wabcut then). AppShell's singleInstance (C++ diagnostics; QML does not read it) is known from
        // here on.
        if (takesLaunches) {
            instance.setReady([&engine] { return engine && !engine->rootObjects().isEmpty(); });
            AppShell::setSingleInstance(instance.listen());
        }
    };
    if (splash)
        QObject::connect(splash, &LaunchSplash::assembled, &app, launch, Qt::SingleShotConnection);
    else
        launch();
    const int code = QGuiApplication::exec();
    wabcut::crash::markCleanExit(); // the next start shows no notice for this run
    // An update the user agreed to install (Help > Check for Updates...): it starts once Wabcut has closed normally, so
    // never during an export (the close waited for those or cancelled them), and replaces this version (MSI upgrade).
    if (code == 0 && !UpdateBridge::launchPendingInstall() && AppShell::restartPending()) {
        // P1 #47: Restart now (a new interface size or cache folder). This one stops taking launches first, so the new
        // Wabcut opens its project itself instead of handing it back here.
        instance.close();
        // 0.7 (card 028): an imported or reset settings bundle is applied now that nothing writes settings any more.
        wabcut::app::SettingsBundle().applyStaged();
        AppShell::launchPendingRestart();
    }
    return code;
}
