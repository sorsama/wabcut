#include "StartupProbe.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QQuickWindow>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

#include <chrono>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace wabcut::app {

namespace {

/// Milliseconds since this process was created (it includes loading the executable and its DLLs).
double msSinceProcessStart() {
#ifdef _WIN32
    FILETIME created{}, exited{}, kernel{}, user{};
    FILETIME now{};
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        GetSystemTimePreciseAsFileTime(&now);
        const auto ticks = [](const FILETIME& f) {
            return (static_cast<unsigned long long>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
        };
        return static_cast<double>(ticks(now) - ticks(created)) / 10'000.0; // 100 ns units
    }
#endif
    static const auto start = std::chrono::steady_clock::now(); // first call: about main()
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

} // namespace

StartupProbe::StartupProbe() {
    m_file = qEnvironmentVariable("WABCUT_PERF_STARTUP");
    m_active = !m_file.isEmpty();
    if (!m_active) {
        return;
    }
    m_quit = qEnvironmentVariable("WABCUT_PERF_STARTUP_QUIT") == QStringLiteral("1");
    const QString settingsDir = qEnvironmentVariable("WABCUT_PERF_SETTINGS_DIR");
    if (!settingsDir.isEmpty()) {
        QDir().mkpath(settingsDir);
        QStandardPaths::setTestModeEnabled(true);
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir);
    }
    mark("main");
}

double StartupProbe::now() const { return msSinceProcessStart(); }

void StartupProbe::mark(const char* name) {
    if (m_active && !m_milestones.contains(QLatin1String(name))) {
        m_milestones.insert(QLatin1String(name), now());
    }
}

void StartupProbe::loaded(QQuickWindow* window) {
    if (!m_active || m_milestones.contains(QStringLiteral("qmlLoaded"))) {
        return;
    }
    mark("qmlLoaded");
    // The first moment the event loop has nothing queued: posted work from loading is done, input would be handled.
    QTimer::singleShot(0, this, [this] {
        mark("firstIdle");
        maybeFinish();
    });
    if (window != nullptr) {
        QObject::connect(window, &QQuickWindow::frameSwapped, this, [this] {
            mark("firstFrame");
            maybeFinish();
        });
    }
    QTimer::singleShot(15'000, this, [this] { finish(); });
}

void StartupProbe::maybeFinish() {
    if (m_milestones.contains(QStringLiteral("firstIdle")) && m_milestones.contains(QStringLiteral("firstFrame"))) {
        finish();
    }
}

void StartupProbe::finish() {
    if (m_done) {
        return;
    }
    m_done = true;
    QJsonObject report{{QStringLiteral("format"), QStringLiteral("wabcut-startup")},
                       {QStringLiteral("version"), 1},
                       {QStringLiteral("milestones"), m_milestones},
                       {QStringLiteral("platform"), QGuiApplication::platformName()}};
    QFile out(m_file);
    if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        out.write(QJsonDocument(report).toJson(QJsonDocument::Indented));
    }
    if (m_quit) {
        QCoreApplication::quit();
    }
}

} // namespace wabcut::app
