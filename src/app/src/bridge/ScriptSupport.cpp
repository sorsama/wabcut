#include "ScriptSupport.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>

namespace wabcut::app::scripts {

namespace {

constexpr int kHeaderLines = 60;
constexpr int kMaxRecent = 12;
constexpr int kMaxRemembered = 200;

/// Plain text for the dialog: no control characters, bounded.
QString cleaned(QString text, int limit) {
    QString out;
    for (const QChar c : text) {
        if (c.unicode() < 0x20 || c == QChar(0x7f) || c == QChar(0x202e) || c == QChar(0x2066) || c == QChar(0x2067) ||
            c == QChar(0x2068) || c == QChar(0x202a) || c == QChar(0x202b) || c == QChar(0x202d) || c == QChar(0x2069) ||
            c == QChar(0x200b) || c == QChar(0x200e) || c == QChar(0x200f)) {
            continue;
        }
        out += c;
    }
    return out.trimmed().left(limit);
}

} // namespace

const QStringList& permissionNames() {
    static const QStringList names{QStringLiteral("Read"), QStringLiteral("EditSequence"), QStringLiteral("EditProject"),
                                   QStringLiteral("MediaAnalysis"), QStringLiteral("Files")};
    return names;
}

ScriptHeader parseScriptHeader(const QString& text) {
    ScriptHeader header;
    static const QRegularExpression field(QStringLiteral("^wabcut-(name|description|permissions)\\s*:\\s*(.*)$"),
                                          QRegularExpression::CaseInsensitiveOption);
    int number = 0;
    for (const QString& raw : text.split(QLatin1Char('\n'))) {
        if (++number > kHeaderLines) {
            break;
        }
        QString line = raw.trimmed();
        if (!line.startsWith(QLatin1Char('#'))) {
            continue;
        }
        line = line.mid(1).trimmed();
        const QRegularExpressionMatch m = field.match(line);
        if (!m.hasMatch()) {
            continue;
        }
        const QString key = m.captured(1).toLower();
        const QString value = m.captured(2);
        if (key == QLatin1String("name")) {
            header.name = cleaned(value, 80);
        } else if (key == QLatin1String("description")) {
            header.description = cleaned(value, 300);
        } else {
            for (const QString& token : value.split(QRegularExpression(QStringLiteral("[,;\\s]+")), Qt::SkipEmptyParts)) {
                const auto canonical = std::find_if(permissionNames().begin(), permissionNames().end(), [&](const QString& n) {
                    return n.compare(token, Qt::CaseInsensitive) == 0;
                });
                if (canonical == permissionNames().end()) {
                    header.unknown.append(cleaned(token, 40));
                } else if (!header.requested.contains(*canonical)) {
                    header.requested.append(*canonical);
                }
            }
        }
    }
    // The dialog's order, whatever the header's.
    QStringList ordered;
    for (const QString& name : permissionNames()) {
        if (header.requested.contains(name)) {
            ordered.append(name);
        }
    }
    header.requested = ordered;
    return header;
}

QString contentHash(const QByteArray& bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

// ---- The grant store ----------------------------------------------------------------------------------------------

ScriptGrantStore::ScriptGrantStore(QString file) : m_file(std::move(file)) { load(); }

void ScriptGrantStore::load() {
    QFile in(m_file);
    if (m_file.isEmpty() || !in.open(QIODevice::ReadOnly)) {
        return;
    }
    const QJsonDocument document = QJsonDocument::fromJson(in.read(4 * 1024 * 1024));
    if (!document.isObject()) {
        return; // a damaged file asks again; it is never trusted
    }
    const QJsonObject root = document.object();
    m_python = root.value(QStringLiteral("python")).toString();
    for (const QJsonValue& v : root.value(QStringLiteral("recent")).toArray()) {
        if (v.isString() && m_recent.size() < kMaxRecent) {
            m_recent.append(v.toString());
        }
    }
    const QJsonObject scripts = root.value(QStringLiteral("scripts")).toObject();
    for (auto it = scripts.begin(); it != scripts.end(); ++it) {
        const QJsonObject o = it.value().toObject();
        Entry entry;
        entry.path = o.value(QStringLiteral("path")).toString();
        for (const QJsonValue& g : o.value(QStringLiteral("granted")).toArray()) {
            if (permissionNames().contains(g.toString())) {
                entry.granted.append(g.toString());
            }
        }
        entry.time = static_cast<qint64>(o.value(QStringLiteral("time")).toDouble());
        m_entries.insert(it.key(), entry);
    }
}

bool ScriptGrantStore::save() const {
    if (m_file.isEmpty()) {
        return false;
    }
    QJsonObject scripts;
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
        scripts.insert(it.key(), QJsonObject{{QStringLiteral("path"), it->path},
                                             {QStringLiteral("granted"), QJsonArray::fromStringList(it->granted)},
                                             {QStringLiteral("time"), static_cast<double>(it->time)}});
    }
    const QJsonObject root{{QStringLiteral("version"), 1},
                           {QStringLiteral("python"), m_python},
                           {QStringLiteral("recent"), QJsonArray::fromStringList(m_recent)},
                           {QStringLiteral("scripts"), scripts}};
    QDir().mkpath(QFileInfo(m_file).absolutePath());
    QSaveFile out(m_file);
    if (!out.open(QIODevice::WriteOnly)) {
        return false;
    }
    out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return out.commit();
}

QStringList ScriptGrantStore::grantsFor(const QString& hash) const {
    const auto it = m_entries.constFind(hash);
    return it == m_entries.constEnd() ? QStringList() : it->granted;
}

void ScriptGrantStore::remember(const QString& hash, const QString& path, const QStringList& granted) {
    Entry entry;
    entry.path = path;
    for (const QString& name : permissionNames()) {
        if (granted.contains(name)) {
            entry.granted.append(name);
        }
    }
    entry.time = QDateTime::currentMSecsSinceEpoch();
    m_entries.insert(hash, entry);
    while (m_entries.size() > kMaxRemembered) {
        auto oldest = m_entries.begin();
        for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
            if (it->time < oldest->time) {
                oldest = it;
            }
        }
        m_entries.erase(oldest);
    }
}

void ScriptGrantStore::forget(const QString& hash) { m_entries.remove(hash); }

QStringList ScriptGrantStore::recent() const { return m_recent; }

void ScriptGrantStore::addRecent(const QString& path) {
    m_recent.removeAll(path);
    m_recent.prepend(path);
    while (m_recent.size() > kMaxRecent) {
        m_recent.removeLast();
    }
}

void ScriptGrantStore::removeRecent(const QString& path) { m_recent.removeAll(path); }

QString ScriptGrantStore::pythonPath() const { return m_python; }

void ScriptGrantStore::setPythonPath(const QString& path) { m_python = path; }

// ---- Python -------------------------------------------------------------------------------------------------------

QString PythonRuntime::display() const {
    if (!ok()) {
        return error;
    }
    return QStringLiteral("Python %1 (%2)").arg(version, QDir::toNativeSeparators(program));
}

bool versionAtLeast(const QString& version, int major, int minor) {
    const QStringList parts = version.split(QLatin1Char('.'));
    if (parts.size() < 2) {
        return false;
    }
    bool okMajor = false;
    bool okMinor = false;
    const int a = parts[0].toInt(&okMajor);
    const int b = parts[1].toInt(&okMinor);
    return okMajor && okMinor && (a > major || (a == major && b >= minor));
}

namespace {

struct Candidate {
    QString program;
    QStringList arguments;
};

/// The version a candidate reports, or "" with `why` filled in.
QString probe(const Candidate& candidate, int timeoutMs, QString& why) {
    QProcess process;
    process.setProgram(candidate.program);
    process.setArguments(candidate.arguments + QStringList{QStringLiteral("-c"),
                                                           QStringLiteral("import sys; print('%d.%d.%d' % sys.version_info[:3])")});
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start();
    if (!process.waitForStarted(timeoutMs)) {
        why = QStringLiteral("%1 could not be started").arg(QDir::toNativeSeparators(candidate.program));
        return {};
    }
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(1000);
        why = QStringLiteral("%1 did not answer").arg(QDir::toNativeSeparators(candidate.program));
        return {};
    }
    const QString out = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
    static const QRegularExpression pattern(QStringLiteral("^\\d+\\.\\d+\\.\\d+$"));
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 || !pattern.match(out).hasMatch()) {
        why = QStringLiteral("%1 is not a working Python").arg(QDir::toNativeSeparators(candidate.program));
        return {};
    }
    return out;
}

#ifdef Q_OS_WIN
/// The Microsoft Store's "python.exe" stubs in WindowsApps are zero-byte aliases that print an install hint.
bool isStoreStub(const QString& path) {
    const QFileInfo info(path);
    return path.contains(QStringLiteral("WindowsApps"), Qt::CaseInsensitive) && info.size() == 0;
}
#endif

} // namespace

PythonRuntime locatePython(const QString& chosen, int timeoutMs) {
    PythonRuntime result;
    QList<Candidate> candidates;
    if (!chosen.isEmpty()) {
        candidates.append({chosen, {}});
    } else {
        if (const QString env = qEnvironmentVariable("WABCUT_PYTHON"); !env.isEmpty()) {
            candidates.append({env, {}});
        }
#ifdef Q_OS_WIN
        if (const QString launcher = QStandardPaths::findExecutable(QStringLiteral("py")); !launcher.isEmpty()) {
            candidates.append({launcher, {QStringLiteral("-3")}});
        }
        for (const char* name : {"python", "python3"}) {
            const QString found = QStandardPaths::findExecutable(QString::fromLatin1(name));
            if (!found.isEmpty() && !isStoreStub(found)) {
                candidates.append({found, {}});
            }
        }
#else
        // A GUI app on macOS starts with a short PATH: look where Homebrew and the system keep Python as well.
        const QStringList extra{QStringLiteral("/opt/homebrew/bin"), QStringLiteral("/usr/local/bin"),
                                QStringLiteral("/usr/bin")};
        for (const char* name : {"python3", "python"}) {
            QString found = QStandardPaths::findExecutable(QString::fromLatin1(name));
            if (found.isEmpty()) {
                found = QStandardPaths::findExecutable(QString::fromLatin1(name), extra);
            }
            if (!found.isEmpty()) {
                candidates.append({found, {}});
            }
        }
#endif
    }
    QStringList notes;
    for (const Candidate& candidate : candidates) {
        QString why;
        const QString version = probe(candidate, timeoutMs, why);
        if (version.isEmpty()) {
            notes.append(why);
            continue;
        }
        if (!versionAtLeast(version, 3, 11)) {
            notes.append(QStringLiteral("Python %1 at %2 is too old (3.11 or newer is needed)")
                             .arg(version, QDir::toNativeSeparators(candidate.program)));
            continue;
        }
        result.program = candidate.program;
        result.arguments = candidate.arguments;
        result.version = version;
        return result;
    }
    if (!chosen.isEmpty()) {
        result.error = notes.isEmpty() ? QStringLiteral("The Python you chose could not be used.") : notes.join(QLatin1Char('\n'));
    } else if (notes.isEmpty()) {
        result.error = QStringLiteral("Wabcut did not find Python. Install Python 3.11 or newer from python.org (Wabcut does not "
                                      "bundle one), or choose the Python to use.");
    } else {
        result.error = QStringLiteral("Wabcut did not find Python 3.11 or newer.\n%1\nInstall one from python.org, or choose the "
                                      "Python to use.")
                           .arg(notes.join(QLatin1Char('\n')));
    }
    return result;
}

} // namespace wabcut::app::scripts
