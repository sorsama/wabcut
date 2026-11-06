#pragma once

// Python scripts (0.7, docs/python.md): what runs a script without the window - reading a script's header, remembering
// what the user allowed it, and finding a Python to run it with.
//
//   * parseScriptHeader: a script may say who it is and what it needs in comment lines near its top:
//         # wabcut-name: Add chapter markers
//         # wabcut-description: One marker per row of chapters.csv.
//         # wabcut-permissions: Read, EditSequence, Files
//     The names are the extension capabilities (docs/extensions.md) plus "Files" (commands that read or write files, inside
//     the project's folder). A script is only ever given what it asks for AND the user allows, as with extensions.
//   * ScriptGrantStore: <app data>/script-permissions.json - the permissions the user allowed, per script content (its
//     SHA-256: an edited script asks again), the recent scripts, and the Python the user chose. Written atomically.
//   * locatePython: Python 3.11 or newer, found when the script is run (no Python is bundled): the user's choice,
//     $WABCUT_PYTHON, the `py` launcher, then python3 / python on the PATH. The Microsoft Store stubs are skipped.
//
// Pure Qt-core code; nothing here touches the project.

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QStringList>

namespace wabcut::app::scripts {

/// The permission names a script can ask for, in the order the dialog lists them.
const QStringList& permissionNames();

struct ScriptHeader {
    QString name;
    QString description;
    QStringList requested;  ///< the permissions asked for, canonical names, in the dialog's order (Read is implied)
    QStringList unknown;    ///< names in the header that are not permissions (reported, never granted)
};

/// Reads the header comments from the first 60 lines of `text`.
ScriptHeader parseScriptHeader(const QString& text);

/// The SHA-256 of a script's bytes, lower-case hex: what a remembered permission is tied to.
QString contentHash(const QByteArray& bytes);

class ScriptGrantStore {
public:
    explicit ScriptGrantStore(QString file);

    /// What the user allowed this exact content before (canonical names), or empty.
    QStringList grantsFor(const QString& hash) const;
    void remember(const QString& hash, const QString& path, const QStringList& granted);
    void forget(const QString& hash);

    /// Recent script paths, newest first (at most 12).
    QStringList recent() const;
    void addRecent(const QString& path);
    void removeRecent(const QString& path);

    /// The Python the user chose, or "".
    QString pythonPath() const;
    void setPythonPath(const QString& path);

    /// Writes the file (a temporary file renamed over it). False when it could not be written.
    bool save() const;
    const QString& file() const { return m_file; }

private:
    struct Entry {
        QString path;
        QStringList granted;
        qint64 time = 0; ///< when it was remembered (ms since the epoch); the oldest go first past the limit
    };
    void load();

    QString m_file;
    QMap<QString, Entry> m_entries; ///< by content hash
    QStringList m_recent;
    QString m_python;
};

/// A Python found for running scripts.
struct PythonRuntime {
    QString program;        ///< the executable ("py", "C:/Python312/python.exe")
    QStringList arguments;  ///< what goes before the script ("-3" for the py launcher)
    QString version;        ///< "3.12.4"
    QString error;          ///< when none was usable: what was found and what to do
    bool ok() const { return !program.isEmpty() && error.isEmpty(); }
    /// "Python 3.12.4 (C:/Python312/python.exe)"
    QString display() const;
};

/// True when "3.12.4" is at least `major`.`minor`.
bool versionAtLeast(const QString& version, int major, int minor);

/// Finds a Python 3.11 or newer. `chosen` is the user's choice (tried alone, so a wrong choice says so). Blocks while it
/// runs each candidate for its version (a second or two): call it off the GUI thread.
PythonRuntime locatePython(const QString& chosen, int timeoutMs = 8000);

} // namespace wabcut::app::scripts
