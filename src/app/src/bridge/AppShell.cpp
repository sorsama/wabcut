#include "AppShell.h"

#include <atomic>

namespace wabcut::app {

namespace {

std::atomic_bool g_singleInstance{false};

} // namespace

AppShell::AppShell(QObject* parent) : QObject(parent) {}

void AppShell::setSingleInstance(bool on) {
    g_singleInstance = on;
}

bool AppShell::singleInstance() const {
    return g_singleInstance;
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

} // namespace wabcut::app
