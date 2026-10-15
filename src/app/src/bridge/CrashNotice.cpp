#include "CrashNotice.h"

#include "../crash/CrashHandler.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QGuiApplication>
#include <QLocale>
#include <QStandardPaths>

namespace wabcut::app {

CrashNotice::CrashNotice(QObject* parent) : QObject(parent) {
    setFolder(crash::installed() ? crash::folder() : defaultFolder());
}

CrashNotice* CrashNotice::create(QQmlEngine*, QJSEngine*) {
    return new CrashNotice;
}

std::filesystem::path CrashNotice::defaultFolder() {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return std::filesystem::path((base + QStringLiteral("/crashes")).toStdU16String());
}

void CrashNotice::setFolder(std::filesystem::path folder) {
    m_folder = std::move(folder);
    crash::prune(m_folder); // the newest 10 reports stay (docs/release.md "Crash reports")
    reload();
}

void CrashNotice::reload() {
    m_reports = crash::pendingReports(m_folder, static_cast<std::uint32_t>(QCoreApplication::applicationPid()));
    emit changed();
}

QString CrashNotice::detail() const {
    if (m_reports.empty()) {
        return {};
    }
    const crash::CrashReport& r = m_reports.front();
    const QDateTime when = QDateTime::fromString(QString::fromStdString(r.time), Qt::ISODate).toLocalTime();
    const QString time = when.isValid() ? QLocale().toString(when, QLocale::ShortFormat) : tr("an earlier session");
    QString text = r.unexpectedExit
                       //: %1 is a date and time
                       ? tr("Wabcut was closed on %1 without saying why (it may have been ended from outside, lost power "
                            "or stopped responding).").arg(time)
                       //: %1 is a date and time, %2 what went wrong (e.g. "access violation")
                       : tr("Wabcut crashed on %1 (%2).").arg(time, QString::fromStdString(r.what));
    if (m_reports.size() > 1) {
        text += QLatin1Char(' ') + tr("%n earlier run(s) also ended unexpectedly.", nullptr, static_cast<int>(m_reports.size() - 1));
    }
    return text;
}

bool CrashNotice::hasDump() const {
    return !m_reports.empty() && !m_reports.front().dump.empty();
}

QUrl CrashNotice::folderUrl() const {
    return QUrl::fromLocalFile(QString::fromStdU16String(m_folder.u16string()));
}

QString CrashNotice::summary() const {
    return m_reports.empty() ? QString() : QString::fromStdString(crash::diagnosticSummary(m_reports.front()));
}

void CrashNotice::copyDiagnosticSummary() {
    if (QClipboard* clipboard = QGuiApplication::clipboard()) {
        clipboard->setText(summary());
    }
}

bool CrashNotice::openFolder() {
    std::error_code ec;
    std::filesystem::create_directories(m_folder, ec);
    return QDesktopServices::openUrl(folderUrl());
}

void CrashNotice::dismiss() {
    crash::acknowledge(m_folder, m_reports);
    m_reports.clear();
    emit changed();
}

void CrashNotice::recordRenderer(const QString& api, const QString& adapter) {
    if (!api.isEmpty()) {
        crash::setDiagnostic("graphicsApi", api.toStdString());
    }
    if (!adapter.isEmpty()) {
        crash::setDiagnostic("gpu", adapter.toStdString());
    }
}

} // namespace wabcut::app
