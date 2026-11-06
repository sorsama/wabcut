#include "GyroAvailability.h"

#include <wabcut/model/MediaLocator.h>

#include <QMetaObject>
#include <QPointer>
#include <QThreadPool>

namespace wabcut::app {

GyroAvailability& GyroAvailability::instance() {
    static GyroAvailability self;
    return self;
}

std::optional<editor::GyroProbe> GyroAvailability::lookup(const MediaItem& item, const std::filesystem::path& projectFile) {
    if (item.info.video.empty() || item.info.isStillImage || item.isGenerator()) {
        return std::nullopt;
    }
    // A file that is not there has no answer - except to a test's probe, which never opens the file.
    const auto file = resolveMediaPath(item.locator, projectFile);
    if (!file && !m_probe) {
        return std::nullopt;
    }
    const std::string utf8 = file ? pathToUtf8(*file) : item.locator.path;
    const QString key = QString::fromStdString(utf8);
    const auto found = m_entries.constFind(key);
    if (found != m_entries.constEnd()) {
        return found->done ? std::optional<editor::GyroProbe>(found->probe) : std::nullopt;
    }
    m_entries.insert(key, Entry{});
    const auto probe = m_probe ? m_probe : std::function<editor::GyroProbe(const std::string&)>(editor::probeGyro);
    if (m_synchronous) {
        const editor::GyroProbe answer = probe(utf8);
        m_entries[key] = Entry{true, answer};
        return answer;
    }
    QThreadPool::globalInstance()->start([self = QPointer<GyroAvailability>(this), key, utf8, probe] {
        editor::GyroProbe answer = probe(utf8);
        if (self) {
            QMetaObject::invokeMethod(self.data(), [self, key, answer = std::move(answer)]() mutable {
                if (self) {
                    self->finish(key, std::move(answer));
                }
            }, Qt::QueuedConnection);
        }
    });
    return std::nullopt;
}

void GyroAvailability::finish(const QString& path, editor::GyroProbe probe) {
    m_entries[path] = Entry{true, std::move(probe)};
    ++m_revision;
    emit changed();
}

void GyroAvailability::setProbeForTests(std::function<editor::GyroProbe(const std::string&)> probe, bool synchronous) {
    m_probe = std::move(probe);
    m_synchronous = synchronous;
    clear();
}

void GyroAvailability::clear() {
    m_entries.clear();
    ++m_revision;
    emit changed();
}

} // namespace wabcut::app
