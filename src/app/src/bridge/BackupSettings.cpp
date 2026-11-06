#include "BackupSettings.h"

#include <QDir>
#include <QSettings>

#include <algorithm>

namespace wabcut::app::backup {

namespace {

const QString kLocationKey = QStringLiteral("project/backupLocation");
const QString kFolderKey = QStringLiteral("project/backupFolder");
const QString kCountKey = QStringLiteral("project/backupCount");
const QString kCopiesKey = QStringLiteral("project/autosaveCopies");

} // namespace

QString normalizedLocation(const QString& value) {
    if (value == QLatin1String("folder") || value == QLatin1String("chosen"))
        return value;
    return QStringLiteral("beside");
}

void chooseDefaultForNewInstall(QSettings& settings) {
    if (settings.contains(kLocationKey))
        return;
    settings.setValue(kLocationKey, settings.allKeys().isEmpty() ? QStringLiteral("folder") : QStringLiteral("beside"));
}

void chooseDefaultForNewInstall() {
    QSettings settings;
    chooseDefaultForNewInstall(settings);
}

wabcut::io::BackupLocation locationFromSettings(const QSettings& settings) {
    using Mode = wabcut::io::BackupLocation::Mode;
    wabcut::io::BackupLocation out;
    // The chosen folder stays known when another place is the setting: copies written there are still found.
    const QString folder = settings.value(kFolderKey).toString().trimmed();
    if (!folder.isEmpty() && QDir::isAbsolutePath(folder))
        out.root = std::filesystem::path(QDir::toNativeSeparators(folder).toStdU16String());
    const QString value = normalizedLocation(settings.value(kLocationKey, QStringLiteral("beside")).toString());
    if (value == QLatin1String("folder")) {
        out.mode = Mode::Folder;
    } else if (value == QLatin1String("chosen") && !out.root.empty()) {
        out.mode = Mode::Chosen;
    }
    return out;
}

int backupCountFromSettings(const QSettings& settings) {
    bool ok = false;
    const int n = settings.value(kCountKey, kDefaultBackupCount).toInt(&ok);
    return std::clamp(ok ? n : kDefaultBackupCount, 0, wabcut::io::kMaxBackupCount);
}

int autosaveCopiesFromSettings(const QSettings& settings) {
    bool ok = false;
    const int n = settings.value(kCopiesKey, kDefaultAutosaveCopies).toInt(&ok);
    return std::clamp(ok ? n : kDefaultAutosaveCopies, 1, wabcut::io::kMaxAutosaveCopies);
}

} // namespace wabcut::app::backup
