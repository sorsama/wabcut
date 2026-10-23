#pragma once

// AppShell (UX rework, stream S; QML singleton `AppShell`): what the editor shell hears from outside its window.
//
//   * Files another launch handed over (single instance, src/SingleInstance.h): a .wabcut double-clicked in File
//     Explorer while Wabcut runs starts a second process, which gives the file to this one and quits. main() brings
//     this window to the front and calls requestOpen(); Main.qml answers openRequested(files) by opening the first
//     project through the normal path - the unsaved-changes prompt (window.whenSaved), Home closing, an error shown
//     when it cannot be opened - exactly like a .wabcut the app was started with (ProjectFile.launchProject()).
//   * `singleInstance`: whether this process takes other launches' files (false in the tests, with --new-instance and
//     in the startup benchmark), for the About box and diagnostics.

#include <QList>
#include <QObject>
#include <QUrl>
#include <QtQml/qqmlregistration.h>

namespace wabcut::app {

class AppShell : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    /// This process listens for other launches' files (see the header).
    Q_PROPERTY(bool singleInstance READ singleInstance CONSTANT FINAL)

public:
    explicit AppShell(QObject* parent = nullptr);

    /// main() sets it before the window loads (default false: the tests).
    static void setSingleInstance(bool on);
    bool singleInstance() const;

    /// Asks the shell to open `files` (Main.qml: openRequested). main() calls it with the files another launch handed
    /// over; tests call it to stand in for that launch.
    Q_INVOKABLE void requestOpen(const QList<QUrl>& files);

signals:
    /// Open these files as if chosen in File > Open: .wabcut projects, absolute file URLs; the first one is opened.
    void openRequested(const QList<QUrl>& files);
};

} // namespace wabcut::app
