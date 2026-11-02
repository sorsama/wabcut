#include "CacheFolders.h"

#include "DiskSpace.h"

#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>

namespace wabcut::app::cachefolders {

namespace {

const QString kMediaFolder = QStringLiteral("cache/mediaFolder");
const QString kAiFolder = QStringLiteral("cache/aiFolder");
const QString kProxyLocation = QStringLiteral("proxies/location");
const QString kProxyFolder = QStringLiteral("proxies/folder");

QString stored(const QString& key) {
    return QSettings().value(key).toString().trimmed();
}

void store(const QString& key, const QString& value) {
    QSettings settings;
    if (value.isEmpty())
        settings.remove(key);
    else
        settings.setValue(key, value);
}

} // namespace

QString localFolder(const QString& urlOrPath) {
    const QString text = urlOrPath.trimmed();
    if (text.isEmpty())
        return {};
    const QUrl url(text);
    const QString path = url.isLocalFile() ? url.toLocalFile() : text;
    return QDir::cleanPath(QDir::fromNativeSeparators(path));
}

QString defaultMediaCacheFolder() {
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/media");
}

QString chosenMediaCacheFolder() {
    return localFolder(stored(kMediaFolder));
}

void setChosenMediaCacheFolder(const QString& folder) {
    const QString local = localFolder(folder);
    store(kMediaFolder, local == defaultMediaCacheFolder() ? QString() : local);
}

QString mediaCacheFolder() {
    const QString chosen = chosenMediaCacheFolder();
    return chosen.isEmpty() ? defaultMediaCacheFolder() : chosen;
}

QString chosenAiCacheFolder() {
    return localFolder(stored(kAiFolder));
}

void setChosenAiCacheFolder(const QString& folder) {
    store(kAiFolder, localFolder(folder));
}

bool applyChosenAiCacheFolder() {
    if (qEnvironmentVariableIsSet("WABCUT_CACHE_DIR"))
        return false; // the user (or a test) chose one already
    const QString folder = chosenAiCacheFolder();
    if (folder.isEmpty())
        return false;
    qputenv("WABCUT_CACHE_DIR", QDir::toNativeSeparators(folder).toUtf8());
    return true;
}

QString proxyLocation() {
    return stored(kProxyLocation) == QLatin1String("folder") ? QStringLiteral("folder") : QStringLiteral("project");
}

void setProxyLocation(const QString& location) {
    store(kProxyLocation, location == QLatin1String("folder") ? QStringLiteral("folder") : QString());
}

QString proxyFolderSetting() {
    return localFolder(stored(kProxyFolder));
}

void setProxyFolderSetting(const QString& folder) {
    store(kProxyFolder, localFolder(folder));
}

QString proxyRoot() {
    return proxyLocation() == QLatin1String("folder") ? proxyFolderSetting() : QString();
}

QString freeSpaceText(const QString& folder) {
    if (folder.isEmpty())
        return {};
    const qint64 free = diskspace::freeBytesAt(folder);
    if (free < 0)
        return {};
    //: %1 is a size ("412 GB"), %2 a drive's name ("Data", "C:/")
    return QCoreApplication::translate("wabcut::app::MediaReadinessBridge", "%1 free on %2")
        .arg(diskspace::bytesText(static_cast<double>(free)), diskspace::volumeNameOf(folder));
}

} // namespace wabcut::app::cachefolders
