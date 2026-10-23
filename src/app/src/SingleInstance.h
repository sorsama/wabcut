#pragma once

// SingleInstance (UX rework, stream S): one Wabcut opens the projects. A launch that names a .wabcut file (File
// Explorer's Open through the installer's opt-in file association runs `wabcut.exe "<file>"`) first offers it to a
// Wabcut already running for this user: when that one takes it, it opens the file in its own window (AppShell's
// openRequested, with the unsaved-changes prompt) and the new process ends at once. Otherwise the new process starts as
// usual and, once its window is up, listens, so later launches hand their files to it - unless another Wabcut already
// answers on the name: that one keeps it (a launch without a project file always starts a Wabcut of its own, as before,
// and must not take the name from the running one). `--new-instance` starts a separate Wabcut that neither hands over
// nor listens; so does the startup benchmark (WABCUT_PERF_STARTUP). The tests never get here (only main() uses it).
//
// The handshake runs over a local socket (a named pipe on Windows) named per user and settings location
// (defaultServerName(), user-only access): the new process sends one line of JSON,
//     {"wabcut":1,"files":["file:///C:/Films/My%20film.wabcut"]}
// and waits up to 3 s for the answer: "ok" when the running Wabcut can open them now (setReady: its window is up), else
// "no". After "ok" the new process confirms with "go" and ends; the running Wabcut opens the files only on that "go".
// So one that answers after the new process gave up (busy or hung for more than 3 s: the new process has started a
// Wabcut of its own meanwhile) finds nobody to confirm and drops the request - the project never opens twice. On
// Windows the sender lets the receiver take the foreground (AllowSetForegroundWindow), so its window can come to the
// front.

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <functional>

class QLocalServer;
class QLocalSocket;
class QWindow;

namespace wabcut::app {

class SingleInstance : public QObject {
    Q_OBJECT

public:
    explicit SingleInstance(QString serverName, QObject* parent = nullptr);
    ~SingleInstance() override;

    /// "wabcut-<16 hex digits>": the same for every Wabcut of this user with the same settings and data locations.
    static QString defaultServerName();
    /// The switch for a separate Wabcut ("--new-instance").
    static QString newInstanceSwitch();
    /// Whether a launch with `arguments` asked for a separate Wabcut (newInstanceSwitch()).
    static bool separateInstance(const QStringList& arguments);
    /// The project a launch with `arguments` hands to a running Wabcut (ProjectLifecycle::launchProjectIn: the first
    /// argument naming an existing .wabcut file, absolute), or an empty URL: none named, or a separate instance asked for.
    static QUrl projectToHandOver(const QStringList& arguments);

    QString serverName() const { return m_name; }
    /// Offers `files` to the Wabcut listening on the name: true when it took them (it answered "ok" and this process
    /// confirmed: it may end). Blocks about `timeoutMs` in all; false at once when nobody listens.
    bool handOver(const QList<QUrl>& files, int timeoutMs = 3000) const;
    /// Whether this Wabcut can open handed-over files now (main(): its window is loaded). While it says no, launches are
    /// answered "no" and start on their own. Unset: always.
    void setReady(std::function<bool()> ready);
    /// Takes the name: later launches' files arrive through filesRequested. False when it cannot listen, or when another
    /// Wabcut already answers on the name (it keeps it; this one then takes no launches).
    bool listen();
    /// Stops listening (before the app restarts itself on another graphics backend: the new process takes over).
    void close();
    bool isListening() const;

signals:
    /// Another launch handed these over (absolute local file URLs, as it sent them).
    void filesRequested(const QList<QUrl>& files);

private:
    void accept();
    void read(QLocalSocket* socket);

    QString m_name;
    QLocalServer* m_server = nullptr;
    std::function<bool()> m_ready;
};

/// Brings `window` to the front: shown, out of the minimized state (back to maximized when it was), raised and
/// activated.
void bringToFront(QWindow* window);

} // namespace wabcut::app
