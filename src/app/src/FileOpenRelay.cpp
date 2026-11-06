#include "FileOpenRelay.h"

#include <QEvent>
#include <QFileOpenEvent>

#include <utility>

namespace wabcut::app {

FileOpenRelay::FileOpenRelay(QObject* parent) : QObject(parent) {}

void FileOpenRelay::setReady(bool ready) {
    m_ready = ready;
    if (m_ready && !m_pending.isEmpty()) {
        const QList<QUrl> files = std::exchange(m_pending, {});
        emit filesRequested(files);
    }
}

bool FileOpenRelay::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() != QEvent::FileOpen)
        return QObject::eventFilter(watched, event);
    const QUrl url = static_cast<QFileOpenEvent*>(event)->url();
    if (url.isLocalFile()) {
        if (m_ready)
            emit filesRequested({url});
        else
            m_pending.append(url);
    }
    return true;
}

} // namespace wabcut::app
