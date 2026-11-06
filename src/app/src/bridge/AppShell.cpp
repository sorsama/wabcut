#include "AppShell.h"

#include "SettingsBundle.h"
#include "../platform/WindowChromeFilter.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QProcess>

#include <atomic>
#include <mutex>

namespace wabcut::app {

namespace {

std::atomic_bool g_singleInstance{false};

std::mutex g_restartMutex;
bool g_restartArmed = false;
QString g_restartProject;

} // namespace

AppShell::AppShell(QObject* parent) : QObject(parent) {}

void AppShell::setSingleInstance(bool on) {
    g_singleInstance = on;
}

bool AppShell::singleInstance() const {
    return g_singleInstance;
}

bool AppShell::systemMenuAvailable() const {
    return chrome::systemMenuAvailable();
}

bool AppShell::snapLayoutsAvailable() const {
    return chrome::snapLayoutsAvailable();
}

bool AppShell::showSystemMenu(QWindow* window, const QPointF& globalPos) {
    return chrome::showSystemMenu(window, globalPos);
}

void AppShell::registerMaximizeButton(QQuickItem* button) {
    chrome::WindowChromeFilter::instance().addMaximizeButton(button);
}

void AppShell::unregisterMaximizeButton(QQuickItem* button) {
    chrome::WindowChromeFilter::instance().removeMaximizeButton(button);
}

void AppShell::requestOpen(const QList<QUrl>& files) {
    QList<QUrl> local;
    for (const QUrl& file : files) {
        if (file.isLocalFile())
            local.append(file);
    }
    if (!local.isEmpty())
        emit openRequested(local);
}

void AppShell::requestRestart() {
    emit restartRequested();
}

void AppShell::armRestart(const QString& project) {
    const QUrl url(project);
    const std::lock_guard lock(g_restartMutex);
    g_restartArmed = true;
    g_restartProject = url.isLocalFile() ? url.toLocalFile() : project;
}

void AppShell::cancelRestart() {
    {
        const std::lock_guard lock(g_restartMutex);
        g_restartArmed = false;
        g_restartProject.clear();
    }
    // 0.7 (card 028): a settings import or reset staged for this restart goes with it.
    SettingsBundle().discardStaged();
}

bool AppShell::restartPending() {
    const std::lock_guard lock(g_restartMutex);
    return g_restartArmed;
}

QStringList AppShell::restartArguments(const QStringList& arguments, const QString& project) {
    QStringList out;
    for (qsizetype i = 1; i < arguments.size(); ++i) {
        const QString& argument = arguments[i];
        if (argument == QLatin1String("--rhi") && i + 1 < arguments.size()) {
            out << argument << arguments[++i]; // an option with its value
            continue;
        }
        if (!argument.startsWith(QLatin1Char('-')))
            continue; // a project or a recording it was started with (0.7: media too): done then; the one open now follows
        out << argument;
    }
    if (!project.isEmpty())
        out << QFileInfo(project).absoluteFilePath();
    return out;
}

bool AppShell::launchPendingRestart() {
    QString project;
    {
        const std::lock_guard lock(g_restartMutex);
        if (!g_restartArmed)
            return false;
        g_restartArmed = false;
        project = g_restartProject;
    }
    return QProcess::startDetached(QCoreApplication::applicationFilePath(),
                                   restartArguments(QCoreApplication::arguments(), project));
}

} // namespace wabcut::app
