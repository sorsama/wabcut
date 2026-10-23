#include "ShellFixture.h"

#include "RhiPolicy.h"

#include <catch2/catch_test_case_info.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QMutex>
#include <QQmlComponent>
#include <QQmlError>
#include <QQuickStyle>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <QtQml/QQmlExtensionPlugin>

#include <cmath>

Q_IMPORT_QML_PLUGIN(WabcutPlugin)

namespace wabcut::test {

namespace {

// ---- Qt message capture -------------------------------------------------------------------------------
// Every warning/critical message (QML engine warnings, binding loops, failed image loads, ...) is recorded so
// a test can assert that loading and interacting with the shell is completely clean.

QMutex g_messagesMutex;
QStringList g_messages;
QtMessageHandler g_previousHandler = nullptr;

void captureMessage(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg) {
        const QString category = context.category ? QString::fromUtf8(context.category) : QStringLiteral("default");
        // The Windows clipboard is one per desktop: test processes run in parallel retry it while another holds it.
        // Not the app's message; it would fail whichever window test happens to touch the clipboard then (and when the
        // retries run out, "Unable to obtain clipboard": the same contention, seen under parallel ctest runs).
        if (category == QLatin1String("qt.qpa.mime") &&
            (message.startsWith(QLatin1String("Retrying to obtain clipboard")) ||
             message.startsWith(QLatin1String("Unable to obtain clipboard"))))
            return;
        QMutexLocker lock(&g_messagesMutex);
        g_messages << QStringLiteral("[%1] %2").arg(category, message);
    }
    if (g_previousHandler)
        g_previousHandler(type, context, message);
}

// ---- Application lifetime ------------------------------------------------------------------------------
// One QGuiApplication per process, created before the first test case. ctest runs every test case in its own
// process, so tagging a case [hidpi125] or [hidpi150] gives it a matching global scale factor.

int g_argc = 1;
char g_argv0[] = "wabcut_app_tests";
char* g_argv[] = {g_argv0, nullptr};
QGuiApplication* g_app = nullptr;
std::unique_ptr<QTemporaryDir> g_settingsDir;
std::unique_ptr<QTemporaryDir> g_cacheDir;
QStringList g_testDataPaths;
double g_scaleFactor = 1.0;
QString g_requestedBackend;

double scaleForTags(const Catch::TestCaseInfo& info) {
    for (const Catch::Tag& tag : info.tags) {
        const std::string name(tag.original.data(), tag.original.size());
        if (name == "hidpi125")
            return 1.25;
        if (name == "hidpi150")
            return 1.5;
    }
    return 1.0;
}

void createApplication(double scaleFactor) {
    // Deterministic pixels: ignore the desktop's display scaling and apply only our own factor.
    qputenv("QT_ENABLE_HIGHDPI_SCALING", "0");
    if (scaleFactor != 1.0)
        qputenv("QT_SCALE_FACTOR", QByteArray::number(scaleFactor));
#if defined(Q_OS_LINUX)
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM") && qEnvironmentVariableIsEmpty("DISPLAY")
        && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
#endif
    g_scaleFactor = scaleFactor;

    // Tests never touch the developer's real locations, and nothing carries over from one test process to the next
    // (M5j): QtCore Settings (Preferences.qml) use INI files in a per-process temporary directory; the standard
    // locations (autosaves and recovery, keymap overrides, AI logs) are Qt's test locations under a per-process
    // application name, removed when the process ends; Wabcut's own cache (analysis, search, models) is a per-process
    // temporary directory. Tests that want recovery files or models seed their own.
    QCoreApplication::setOrganizationName(QStringLiteral("WabcutTests"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("tests.wabcut.org"));
    const QString testName = QStringLiteral("wabcut_app_tests-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    QCoreApplication::setApplicationName(testName);
    QCoreApplication::setApplicationVersion(QStringLiteral(WABCUT_VERSION));
    QStandardPaths::setTestModeEnabled(true);
    g_settingsDir = std::make_unique<QTemporaryDir>();
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, g_settingsDir->path());
    g_cacheDir = std::make_unique<QTemporaryDir>();
    qputenv("WABCUT_CACHE_DIR", QDir::toNativeSeparators(g_cacheDir->path()).toUtf8());

    g_previousHandler = qInstallMessageHandler(captureMessage);
    g_app = new QGuiApplication(g_argc, g_argv);
    // Capture and validate exact owned directories while the application identity is alive.
    for (const auto location : {QStandardPaths::AppLocalDataLocation, QStandardPaths::AppDataLocation,
                                QStandardPaths::AppConfigLocation, QStandardPaths::CacheLocation}) {
        const QString path = QDir::cleanPath(QStandardPaths::writableLocation(location));
        if (QDir::isAbsolutePath(path) && QDir(path).dirName() == testName)
            g_testDataPaths.append(path);
    }
    g_testDataPaths.removeDuplicates();
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    // Same backend policy as the app (main.cpp), so the GPU pill reports what the app would use.
    using namespace wabcut::app;
    const RhiChoice choice = resolveRhiBackend({}, qgetenv("QSG_RHI_BACKEND"), currentRhiPlatform());
    g_requestedBackend = rhiBackendDisplayName(applyRhiBackend(choice));
}

class QtApplicationListener final : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;

    void testCaseStarting(const Catch::TestCaseInfo& info) override {
        if (!g_app)
            createApplication(scaleForTags(info));
    }

    void testRunEnded(const Catch::TestRunStats&) override {
        delete g_app;
        g_app = nullptr;
        qInstallMessageHandler(g_previousHandler);
        g_settingsDir.reset();
        g_cacheDir.reset();
        for (const QString& path : g_testDataPaths)
            QDir(path).removeRecursively();
        g_testDataPaths.clear();
    }
};

} // namespace

double appScaleFactor() {
    return g_scaleFactor;
}

QString requestedRhiBackend() {
    return g_requestedBackend;
}

void clearMessages() {
    QMutexLocker lock(&g_messagesMutex);
    g_messages.clear();
}

QStringList takeMessages() {
    QMutexLocker lock(&g_messagesMutex);
    QStringList out;
    out.swap(g_messages);
    return out;
}

std::string join(const QStringList& lines) {
    return lines.join(QLatin1Char('\n')).toStdString();
}

void spin(int milliseconds) {
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

bool waitFor(const std::function<bool()>& condition, int timeoutMs) {
    QElapsedTimer timer;
    timer.start();
    while (!condition()) {
        if (timer.elapsed() > timeoutMs)
            return false;
        spin(10);
    }
    return true;
}

bool sameColor(const QColor& a, const QColor& b, int tolerance) {
    return std::abs(a.red() - b.red()) <= tolerance && std::abs(a.green() - b.green()) <= tolerance
        && std::abs(a.blue() - b.blue()) <= tolerance && std::abs(a.alpha() - b.alpha()) <= tolerance;
}

std::string describe(const QColor& c) {
    return c.name(QColor::HexArgb).toStdString();
}

QString outputDir() {
    const QString dir = QStringLiteral(WABCUT_TEST_OUTPUT_DIR "/ui");
    QDir().mkpath(dir);
    return dir;
}

void writeOutputFile(const QString& name, const QString& text) {
    QFile file(outputDir() + QLatin1Char('/') + name);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        file.write(text.toUtf8());
}

void resetSettings() {
    QSettings settings;
    settings.clear();
    settings.sync();
}

QColor pixelAt(const QImage& image, const QQuickItem* item, QPointF local, double dpr) {
    const QPointF scene = item->mapToScene(local);
    return image.pixelColor(int(std::lround(scene.x() * dpr)), int(std::lround(scene.y() * dpr)));
}

Shell::Shell() {
    QQmlComponent component(&engine, "Wabcut", "Main");
    if (component.isError()) {
        for (const QQmlError& e : component.errors())
            errors << e.toString();
        return;
    }
    // visible=false overrides Main.qml's `visible: true`: the window is never shown on screen.
    QObject* created = component.createWithInitialProperties({{QStringLiteral("visible"), false}});
    for (const QQmlError& e : component.errors())
        errors << e.toString();
    window.reset(qobject_cast<QQuickWindow*>(created));
    if (!window) {
        delete created;
        return;
    }
    window->create(); // native window handle for the offscreen grab; still hidden
    resize(kShotWidth, kShotHeight);
    theme = engine.singletonInstance<QObject*>("Wabcut", "Theme");
    preferences = engine.singletonInstance<QObject*>("Wabcut", "Preferences");
}

void Shell::resize(int width, int height) {
    window->resize(width, height);
    window->contentItem()->setSize(QSizeF(width, height));
}

QQuickItem* Shell::item(const char* objectName) const {
    return window ? window->findChild<QQuickItem*>(QLatin1String(objectName)) : nullptr;
}

QObject* Shell::object(const char* objectName) const {
    return window ? window->findChild<QObject*>(QLatin1String(objectName)) : nullptr;
}

QColor Shell::themeColor(const char* token) const {
    return theme->property(token).value<QColor>();
}

void Shell::setAppearance(bool dark, const char* accent) {
    theme->setProperty("dark", dark);
    theme->setProperty("accentName", QString::fromLatin1(accent));
}

bool Shell::settle(bool dark, const QColor& accentFill) {
    // Design language v2: a matte black canvas (dark) / "daylight" (light).
    const QColor canvas = dark ? QColor(0x00, 0x00, 0x00) : QColor(0xED, 0xED, 0xF0);
    const bool ok = waitFor([&] {
        return sameColor(themeColor("canvas"), canvas, 0) && sameColor(themeColor("accentFill"), accentFill, 0);
    });
    spin(400);
    return ok;
}

QImage Shell::grab() const {
    return window->grabWindow();
}

} // namespace wabcut::test

CATCH_REGISTER_LISTENER(wabcut::test::QtApplicationListener)
