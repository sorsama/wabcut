#pragma once

// FileOpenRelay (macOS first-class): Finder does not start an application with the file as an argument. A double-click on
// a .wabcut, Open With, or a file dropped on the Dock icon reaches the running application as a QFileOpenEvent, also
// right after launch, before any window exists. This is the application-wide event filter that catches those events,
// keeps the files until the window is up (setReady(true)) and then hands them to the same path every other launch uses
// (main(): bring the window to the front, AppShell::requestOpen, which Main.qml answers with the unsaved-changes prompt).
// Windows and Linux never deliver the event (they pass the file as an argument: SingleInstance), so nothing happens there.

#include <QList>
#include <QObject>
#include <QUrl>

class QEvent;

namespace wabcut::app {

class FileOpenRelay : public QObject {
    Q_OBJECT

public:
    explicit FileOpenRelay(QObject* parent = nullptr);

    /// Whether the window can take files now. Files that arrived earlier are emitted, in order, once it can.
    void setReady(bool ready);
    /// Files that arrived and wait for setReady(true).
    QList<QUrl> pending() const { return m_pending; }

    /// Takes QFileOpenEvent (local files only: a URL of another scheme is ignored, but still reported as handled); every
    /// other event passes on.
    bool eventFilter(QObject* watched, QEvent* event) override;

signals:
    void filesRequested(const QList<QUrl>& files);

private:
    bool m_ready = false;
    QList<QUrl> m_pending;
};

} // namespace wabcut::app
