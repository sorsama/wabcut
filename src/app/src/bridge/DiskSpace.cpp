#include "DiskSpace.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QStorageInfo>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>

namespace wabcut::app::diskspace {

namespace {

QString tx(const char* text) {
    return QCoreApplication::translate("DiskSpace", text);
}

std::mutex& providerMutex() {
    static std::mutex m;
    return m;
}

std::function<qint64(const QString&)>& provider() {
    static std::function<qint64(const QString&)> p;
    return p;
}

/// A local path from a path or a file URL, cleaned ("" when neither).
QString localPath(const QString& pathOrUrl) {
    if (pathOrUrl.isEmpty())
        return {};
    const QUrl url(pathOrUrl);
    const QString path = url.isLocalFile() ? url.toLocalFile() : QDir::fromNativeSeparators(pathOrUrl);
    return QDir::isAbsolutePath(path) ? QDir::cleanPath(path) : QString();
}

/// The nearest existing file or folder at or above `path` ("" when none).
QString existingAt(const QString& path) {
    QString at = path;
    for (int guard = 0; guard < 256 && !at.isEmpty(); ++guard) {
        if (QFileInfo::exists(at))
            return at;
        const QString parent = QFileInfo(at).absolutePath();
        if (parent == at)
            break;
        at = parent;
    }
    return {};
}

} // namespace

qint64 freeBytesAt(const QString& path) {
    {
        std::lock_guard lock(providerMutex());
        if (provider())
            return provider()(path);
    }
    const QString existing = existingAt(localPath(path));
    if (existing.isEmpty())
        return -1;
    // The Export panel asks for every preset tile on each edit: a volume's answer is kept for two seconds.
    struct Cached {
        qint64 bytes = -1;
        QElapsedTimer age;
    };
    static std::mutex cacheMutex;
    static std::map<QString, Cached> cache;
    {
        std::lock_guard lock(cacheMutex);
        if (const auto it = cache.find(existing); it != cache.end() && it->second.age.isValid() && it->second.age.elapsed() < 2000)
            return it->second.bytes;
    }
    const QStorageInfo storage(existing);
    const qint64 bytes = storage.isValid() && storage.isReady() ? storage.bytesAvailable() : -1;
    std::lock_guard lock(cacheMutex);
    if (cache.size() > 64)
        cache.clear();
    Cached& entry = cache[existing];
    entry.bytes = bytes;
    entry.age.start();
    return bytes;
}

QString volumeNameOf(const QString& path) {
    const QString local = localPath(path);
    const QString existing = existingAt(local);
    if (!existing.isEmpty()) {
        const QStorageInfo storage(existing);
        if (storage.isValid()) {
            if (!storage.displayName().isEmpty() && storage.displayName() != storage.rootPath())
                return storage.displayName();
            if (!storage.rootPath().isEmpty())
                return QDir::toNativeSeparators(storage.rootPath());
        }
    }
    return QDir::toNativeSeparators(local.isEmpty() ? path : local);
}

void setFreeBytesProviderForTests(std::function<qint64(const QString&)> next) {
    std::lock_guard lock(providerMutex());
    provider() = std::move(next);
}

QString bytesText(double bytes) {
    if (bytes < 1e6)
        return tx("%1 KB").arg(std::max(1.0, std::round(bytes / 1e3)));
    if (bytes < 1e9)
        return tx("%1 MB").arg(std::round(bytes / 1e6));
    return tx("%1 GB").arg(bytes / 1e9, 0, 'f', bytes < 1e11 ? 1 : 0);
}

QString lowSpaceMessage(const QString& path, double needed, const QString& what) {
    if (needed <= 0)
        return {};
    const qint64 free = freeBytesAt(path);
    if (free < 0 || needed * kMargin <= static_cast<double>(free))
        return {};
    //: Low disk space, e.g. "Only 1.2 GB is free on Data; about 3.4 GB is needed for the export."
    return tx("Only %1 is free on %2; about %3 is needed for %4.")
        .arg(bytesText(static_cast<double>(free)), volumeNameOf(path), bytesText(needed), what);
}

} // namespace wabcut::app::diskspace
