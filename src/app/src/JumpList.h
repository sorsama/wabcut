#pragma once

// JumpList (0.7, card 030): the recent and starred projects in the Windows taskbar button's right-click menu.
//
//   * jumpListEntries(recent, starred, limit): the pure part - which projects, in which order, with which titles:
//     starred first (in their order), then the recent ones (newest first), without duplicates, without files that are
//     gone, at most `limit` (10). Two projects with the same name get their folder's name in the title.
//   * The Windows part (#ifdef _WIN32, shell32 / ole32 only): ICustomDestinationList with a "Starred" and a "Recent
//     projects" category of IShellLink items. Each item starts this executable with the project's path as its argument
//     (SingleInstance hands that to the running Wabcut), so no file association is needed. No explicit AppUserModelID is
//     set: the taskbar button of the running Wabcut and the installer's shortcut keep the one Windows derives, and the
//     jump list belongs to that. Items the user removed from the list ("Remove from this list") are not put back.
//   * JumpList (QML singleton `JumpList`): reads home/recentProjects and home/starred, rebuilds the list one second after
//     the last change (Main.qml calls refresh() when Home.recent or the starred projects change), on the GUI thread, and
//     deletes it when the setting jumpList/enabled (Settings > General > Projects, default on) is off.
//   * Elsewhere the list is a no-op (`available` is false).
//
// Not done: tasks ("New Project", "Home") - SingleInstance::handOver carries file URLs only, so tasks need new launch
// switches and handover messages.

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include <functional>
#include <vector>

namespace wabcut::app {

struct JumpListEntry {
    QString title;   ///< what the jump list shows ("Film")
    QString path;    ///< the project file, native separators
    QString tooltip; ///< its full path
    bool starred = false;
    bool operator==(const JumpListEntry&) const = default;
};

/// The jump list's projects: `starred` first, then `recent`, no file twice (compared as Windows compares paths),
/// files `exists` says are gone left out, at most `limit` (0 = none). Paths may be written with either separator.
std::vector<JumpListEntry> jumpListEntries(const QStringList& recent, const QStringList& starred, int limit = 10,
                                           const std::function<bool(const QString&)>& exists = {});

class JumpList : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(JumpList)
    QML_SINGLETON

    /// The platform has jump lists (Windows).
    Q_PROPERTY(bool available READ available CONSTANT FINAL)
    /// Settings > General > Projects: show recent and starred projects in the taskbar jump list (QSettings
    /// jumpList/enabled, default on). Off deletes the list.
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged FINAL)

public:
    explicit JumpList(QObject* parent = nullptr);

    bool available() const;
    bool enabled() const;
    void setEnabled(bool on);

    /// Rebuilds the list about a second from now (the last call wins).
    Q_INVOKABLE void refresh();
    /// Rebuilds the list now from the stored recent and starred projects. True when a list was written (or deleted).
    Q_INVOKABLE bool refreshNow();

    /// Tests: what writes the list (default: the Windows API). `clear` runs for a disabled jump list.
    struct Backend {
        std::function<bool(const std::vector<JumpListEntry>&)> write;
        std::function<bool()> clear;
    };
    void setBackend(Backend backend) { m_backend = std::move(backend); }
    /// The entries refreshNow() last wrote.
    const std::vector<JumpListEntry>& lastEntries() const { return m_last; }

    /// The recent projects' paths in QSettings home/recentProjects, newest first, and the starred ones in home/starred.
    static QStringList storedRecent();
    static QStringList storedStarred();

signals:
    void enabledChanged();

private:
    QTimer m_debounce;
    Backend m_backend;
    std::vector<JumpListEntry> m_last;
};

#ifdef _WIN32
/// Windows: replaces the jump list with the two categories of `entries` for this executable; false when Windows refused.
bool writeWindowsJumpList(const std::vector<JumpListEntry>& entries);
/// Windows: deletes the jump list (ICustomDestinationList::DeleteList).
bool deleteWindowsJumpList();
#endif

} // namespace wabcut::app
