#include "CrashNotice.h"

#include "../crash/CrashHandler.h"
#include "../crash/SystemInfo.h"
#include "UiScale.h"

#include <wabcut/media/VideoDecoder.h>

#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QGuiApplication>
#include <QLocale>
#include <QScreen>
#include <QStandardPaths>
#include <QStringList>
#include <QUrl>

#include <map>
#include <string>

namespace wabcut::app {

namespace {

QString g_productVersion;
/// The longest bug report address Wabcut builds (GitHub and browsers cope with much more; this keeps it shareable).
constexpr qsizetype kMaxIssueUrl = 6000;

} // namespace

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

void CrashNotice::setProductVersion(const QString& version) {
    g_productVersion = version;
}

QString CrashNotice::systemInfo(const QString& renderer) const {
    // What this computer reports does not change while Wabcut runs (and asking for the graphics card takes a moment).
    static const std::map<std::string, std::string> system = crash::systemDiagnostics();
    const auto field = [&](const char* key) {
        const auto it = system.find(key);
        return it == system.end() ? QString() : QString::fromStdString(it->second);
    };
    QString version = g_productVersion.isEmpty() ? QCoreApplication::applicationVersion() : g_productVersion;
    if (version.isEmpty()) {
        version = tr("unknown");
    }
    QStringList lines;
    lines << QStringLiteral("Wabcut: ") + version;
    if (const QString os = field("os"); !os.isEmpty()) {
        lines << QStringLiteral("OS: ") + os;
    }
    if (const QString cpu = field("cpu"); !cpu.isEmpty()) {
        lines << QStringLiteral("CPU: ") + cpu;
    }
    if (const QString gpu = field("gpu"); !gpu.isEmpty()) {
        const QString driver = field("driver");
        lines << QStringLiteral("GPU: ") + gpu + (driver.isEmpty() ? QString() : QStringLiteral(" (driver ") + driver + QLatin1Char(')'));
    }
    if (!renderer.trimmed().isEmpty()) {
        lines << QStringLiteral("Graphics: ") + renderer.trimmed();
    }
    QStringList decoders;
    for (const std::string& backend : wabcut::media::hardwareBackendOrder()) {
        decoders << QString::fromStdString(backend);
    }
    decoders << QStringLiteral("software");
    lines << QStringLiteral("Video decoding: ") + decoders.join(QStringLiteral(", "));
    lines << QStringLiteral("Qt: ") + QString::fromLatin1(qVersion());
    lines << QStringLiteral("Interface size: %1 %").arg(appliedUiScale());
    lines << QStringLiteral("Screens: %1").arg(QGuiApplication::screens().size());
    return QString::fromStdString(crash::redact(lines.join(QLatin1Char('\n')).toStdString()));
}

QString CrashNotice::bugReportUrl(const QString& info) const {
    const QString version = g_productVersion.isEmpty() ? QCoreApplication::applicationVersion() : g_productVersion;
    const auto address = [&](const QString& system) {
        return QStringLiteral("https://github.com/sorsama/wabcut/issues/new?template=bug-report.yml&version=") +
               QString::fromLatin1(QUrl::toPercentEncoding(version)) + QStringLiteral("&system=") +
               QString::fromLatin1(QUrl::toPercentEncoding(system));
    };
    // Browsers and GitHub take long addresses, but not unboundedly: drop trailing lines until it fits.
    QStringList lines = info.split(QLatin1Char('\n'));
    QString url = address(lines.join(QLatin1Char('\n')));
    while (url.size() > kMaxIssueUrl && lines.size() > 1) {
        lines.removeLast();
        url = address(lines.join(QLatin1Char('\n')));
    }
    return url;
}

void CrashNotice::copyText(const QString& text) {
    if (QClipboard* clipboard = QGuiApplication::clipboard()) {
        clipboard->setText(text);
    }
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
