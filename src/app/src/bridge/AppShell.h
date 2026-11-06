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
//   * P1 #47, Restart now (Settings > Appearance > Interface size, Media & cache): requestRestart() asks Main.qml, which
//     arms the restart (armRestart, with the open project) and closes the window through the normal path - the
//     unsaved-changes prompt for every open project, running exports - and calls it off (cancelRestart) when the user
//     keeps Wabcut open. main() starts the new Wabcut after the event loop ended (launchPendingRestart), reopening that
//     project.

#include <QList>
#include <QObject>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QtQml/qqmlregistration.h>

class QQuickItem;
class QWindow;
// The invokables take pointers to these: moc's code needs them complete.
Q_MOC_INCLUDE(<QQuickItem>)
Q_MOC_INCLUDE(<QWindow>)

namespace wabcut::app {

class AppShell : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    /// This process listens for other launches' files (see the header).
    Q_PROPERTY(bool singleInstance READ singleInstance CONSTANT FINAL)
    /// 0.7 (card 017): the window's system menu can be popped (showSystemMenu; Windows). False elsewhere: the title bar
    /// then offers no right-click menu and Alt+Space does nothing.
    Q_PROPERTY(bool systemMenuAvailable READ systemMenuAvailable CONSTANT FINAL)
    /// 0.7 (card 017): the maximize button shows Windows 11's Snap Layouts flyout (registerMaximizeButton).
    Q_PROPERTY(bool snapLayoutsAvailable READ snapLayoutsAvailable CONSTANT FINAL)

public:
    explicit AppShell(QObject* parent = nullptr);

    bool systemMenuAvailable() const;
    bool snapLayoutsAvailable() const;
    /// 0.7 (card 017): pops `window`'s system menu (Restore, Move, Size, Minimize, Maximize, Close) at `globalPos` (global
    /// pixels: a pointer point's globalPosition, Item.mapToGlobal) and carries out the choice. The title bar's right-click
    /// and Alt+Space call it. False where there is no system menu (the QML does not offer it there).
    Q_INVOKABLE bool showSystemMenu(QWindow* window, const QPointF& globalPos);
    /// 0.7 (card 017): the window's maximize pill (WindowControls) tells the system where its maximize button is, so
    /// hovering it shows the Snap Layouts flyout (src/platform/WindowChromeFilter.h). The item needs `nativeHovered` and
    /// `nativePressed` properties and a clicked() signal. A no-op off Windows 11.
    Q_INVOKABLE void registerMaximizeButton(QQuickItem* button);
    Q_INVOKABLE void unregisterMaximizeButton(QQuickItem* button);

    /// main() sets it before the window loads (default false: the tests).
    static void setSingleInstance(bool on);
    bool singleInstance() const;

    /// Asks the shell to open `files` (Main.qml: openRequested). main() calls it with the files another launch handed
    /// over; tests call it to stand in for that launch.
    Q_INVOKABLE void requestOpen(const QList<QUrl>& files);

    /// P1 #47: Restart now (see the header). Main.qml answers restartRequested().
    Q_INVOKABLE void requestRestart();
    /// Main.qml, before it closes the window for a restart: the restarted Wabcut opens `project` (a local path or file
    /// URL; "" = none, Home).
    Q_INVOKABLE void armRestart(const QString& project);
    /// The close was given up (Cancel at "Save changes?", Keep Wabcut Open): no restart.
    Q_INVOKABLE void cancelRestart();
    /// A restart is armed (main() and tests).
    static bool restartPending();
    /// The new process's arguments: this one's options (`arguments` as QCoreApplication::arguments(), the program
    /// first), without the files it was started with (projects and, since 0.7, media: they were opened or imported
    /// then), then `project` when there is one.
    static QStringList restartArguments(const QStringList& arguments, const QString& project);
    /// main(), after the event loop: starts the new Wabcut when a restart is armed (and disarms it). True when it started.
    static bool launchPendingRestart();

signals:
    /// Open these files as if chosen in File > Open: .wabcut projects, absolute file URLs; the first one is opened.
    void openRequested(const QList<QUrl>& files);
    /// P1 #47: the user asked for Restart now.
    void restartRequested();
};

} // namespace wabcut::app
