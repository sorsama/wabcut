#include "SingleInstance.h"

#include "ProjectLifecycle.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QSettings>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QWindow>

#include <algorithm>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace wabcut::app {

namespace {

constexpr int kVersion = 1;
constexpr qint64 kMaxMessage = 64 * 1024;
/// A sender that connected but never finished is dropped after this long.
constexpr int kStaleMs = 10'000;
/// How long a launch's "go" may take to go out once the running Wabcut said "ok" (even at the end of the wait).
constexpr int kConfirmMs = 1000;
/// How long listen() waits for a Wabcut that may hold the name to answer.
constexpr int kProbeMs = 500;
/// A launch that waits for the Wabcut starting beside it offers its files again this often.
constexpr int kRetryMs = 150;
/// A steady stream of hand-overs is cut into batches of at most this many batch windows.
constexpr int kBatchCap = 5;

// Where a hand-over is on the receiving side (a property of its socket).
const char* const kStage = "wabcutStage";
const char* const kFiles = "wabcutFiles";
enum Stage : int {
    Request = 0, ///< waiting for the request line
    Confirm = 1, ///< answered "ok": waiting for the sender's "go"
    Done = 2,    ///< answered "no", confirmed, or given up
};

/// Whether a Wabcut answers on `name` (it accepts a connection).
bool answers(const QString& name) {
    QLocalSocket probe;
    probe.connectToServer(name);
    const bool connected = probe.waitForConnected(kProbeMs);
    probe.abort();
    return connected;
}

} // namespace

SingleInstance::SingleInstance(QString serverName, QObject* parent) : QObject(parent), m_name(std::move(serverName)) {}

SingleInstance::~SingleInstance() = default;

QString SingleInstance::defaultServerName() {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QDir::homePath().toUtf8());
    hash.addData(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation).toUtf8());
    hash.addData(QSettings().fileName().toUtf8());
    return QStringLiteral("wabcut-") + QString::fromLatin1(hash.result().toHex().left(16));
}

QString SingleInstance::newInstanceSwitch() {
    return QStringLiteral("--new-instance");
}

bool SingleInstance::separateInstance(const QStringList& arguments) {
    return arguments.mid(1).contains(newInstanceSwitch(), Qt::CaseInsensitive);
}

QUrl SingleInstance::projectToHandOver(const QStringList& arguments) {
    return separateInstance(arguments) ? QUrl() : ProjectLifecycle::launchProjectIn(arguments);
}

QList<QUrl> SingleInstance::filesToHandOver(const QStringList& arguments) {
    return separateInstance(arguments) ? QList<QUrl>() : ProjectLifecycle::launchFilesIn(arguments);
}

bool SingleInstance::handOver(const QList<QUrl>& files, int timeoutMs) const {
    if (files.isEmpty())
        return false;
    QElapsedTimer clock;
    clock.start();
    const auto left = [&] { return std::max(0, timeoutMs - static_cast<int>(clock.elapsed())); };
    QLocalSocket socket;
    socket.connectToServer(m_name);
    if (!socket.waitForConnected(left()))
        return false; // nobody listens (or a Wabcut too busy to answer): start as usual
#ifdef _WIN32
    // The receiver's window may come to the front: Windows lets only the foreground process (this one, just started
    // from File Explorer) pass that on.
    AllowSetForegroundWindow(ASFW_ANY);
#endif
    QJsonArray list;
    for (const QUrl& file : files)
        list.append(file.toString(QUrl::FullyEncoded));
    const QByteArray message =
        QJsonDocument(QJsonObject{{QStringLiteral("wabcut"), kVersion}, {QStringLiteral("files"), list}})
            .toJson(QJsonDocument::Compact) +
        '\n';
    socket.write(message);
    if (!socket.waitForBytesWritten(left()))
        return false;
    QByteArray reply;
    while (!reply.contains('\n')) {
        if (socket.bytesAvailable() == 0 && !socket.waitForReadyRead(left()))
            return false; // no answer in time: this launch starts on its own (and never confirms, so it stays that way)
        reply += socket.readAll();
        if (reply.size() > 64)
            return false;
    }
    if (!reply.startsWith("ok\n"))
        return false;
    // Taken: confirm. The running Wabcut opens the files only now.
    socket.write("go\n");
    const bool confirmed = socket.waitForBytesWritten(std::max(left(), kConfirmMs));
    socket.disconnectFromServer();
    return confirmed;
}

bool SingleInstance::claimLaunch() {
    if (m_lock && m_lock->isLocked())
        return true;
    auto lock = std::make_unique<QLockFile>(QDir::temp().filePath(m_name + QStringLiteral(".launch.lock")));
    // Only a lock whose process is gone counts as stale (never one that is merely old: a Wabcut runs for days).
    lock->setStaleLockTime(0);
    if (!lock->tryLock(0))
        return false;
    m_lock = std::move(lock);
    return true;
}

bool SingleInstance::holdsLaunch() const {
    return m_lock && m_lock->isLocked();
}

bool SingleInstance::handOverOrWait(const QList<QUrl>& files, int waitMs, int timeoutMs) {
    if (files.isEmpty())
        return false;
    QElapsedTimer clock;
    clock.start();
    for (;;) {
        if (handOver(files, timeoutMs))
            return true;
        // Nobody took them. The first launch to get the lock is the Wabcut the others hand their files to (it opens its
        // own files itself, and listens once its window is up).
        if (claimLaunch())
            return false;
        if (clock.elapsed() >= waitMs)
            return false; // the one that holds the lock never answered (hung, or still starting): start on your own
        QThread::msleep(static_cast<unsigned long>(std::min<qint64>(kRetryMs, waitMs - clock.elapsed())));
    }
}

void SingleInstance::setBatchWindow(int ms) {
    m_batchMs = std::max(0, ms);
}

void SingleInstance::queue(const QList<QUrl>& files) {
    if (m_batchMs <= 0) {
        emit filesRequested(files);
        return;
    }
    if (m_batch.isEmpty())
        m_batchClock.start();
    for (const QUrl& file : files) {
        if (!m_batch.contains(file))
            m_batch.append(file);
    }
    if (!m_batchTimer) {
        m_batchTimer = new QTimer(this);
        m_batchTimer->setSingleShot(true);
        connect(m_batchTimer, &QTimer::timeout, this, &SingleInstance::flush);
    }
    // Every arrival restarts the wait; a steady stream of them still ends after kBatchCap windows.
    const qint64 cap = static_cast<qint64>(m_batchMs) * kBatchCap;
    m_batchTimer->start(static_cast<int>(std::max<qint64>(1, std::min<qint64>(m_batchMs, cap - m_batchClock.elapsed()))));
}

void SingleInstance::flush() {
    if (m_batch.isEmpty())
        return;
    const QList<QUrl> files = std::exchange(m_batch, {});
    emit filesRequested(files);
}

void SingleInstance::setReady(std::function<bool()> ready) {
    m_ready = std::move(ready);
}

bool SingleInstance::listen() {
    if (m_server && m_server->isListening())
        return true;
    // A Wabcut that answers on the name keeps it. Taking it over (Unix: a new socket file in its place; Windows: a second
    // server on the same pipe name) would leave the running one unreachable, or share the launches between the two.
    if (answers(m_name))
        return false;
    if (!m_server) {
        m_server = new QLocalServer(this);
        m_server->setSocketOptions(QLocalServer::UserAccessOption);
        connect(m_server, &QLocalServer::newConnection, this, &SingleInstance::accept);
    }
    if (m_server->listen(m_name))
        return true;
#ifndef _WIN32
    // A socket file a crashed Wabcut left: nobody answered on it (above), so the name is free to take.
    if (m_server->serverError() == QAbstractSocket::AddressInUseError) {
        QLocalServer::removeServer(m_name);
        return m_server->listen(m_name);
    }
#endif
    return false;
}

void SingleInstance::close() {
    if (m_server)
        m_server->close();
    // The Wabcut that restarts itself must not find the launch lock held by the one that is ending.
    if (m_lock)
        m_lock->unlock();
    m_lock.reset();
}

bool SingleInstance::isListening() const {
    return m_server && m_server->isListening();
}

void SingleInstance::accept() {
    while (QLocalSocket* socket = m_server->nextPendingConnection()) {
        connect(socket, &QLocalSocket::readyRead, this, [this, socket] { read(socket); });
        // The sender closes its end once it confirmed (or gave up); the receiver never cuts it off first (unread data in
        // a pipe is discarded when the server disconnects). What it sent last is read before the socket goes.
        connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
            if (socket->isReadable())
                read(socket);
            socket->setProperty(kStage, Done);
            socket->deleteLater();
        });
        QTimer::singleShot(kStaleMs, socket, [socket] { socket->abort(); });
        read(socket); // the message may be there already
    }
}

void SingleInstance::read(QLocalSocket* socket) {
    while (socket->property(kStage).toInt() != Done && socket->canReadLine()) {
        const QByteArray line = socket->readLine(kMaxMessage).trimmed();
        if (socket->property(kStage).toInt() == Confirm) {
            // The sender is still there and confirms: the files open now. Anything else (or nothing: it gave up and
            // started a Wabcut of its own) drops them.
            socket->setProperty(kStage, Done);
            const QList<QUrl> files = socket->property(kFiles).value<QList<QUrl>>();
            if (line == "go" && !files.isEmpty())
                queue(files);
            return;
        }
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(line, &error);
        QList<QUrl> files;
        if (error.error == QJsonParseError::NoError && document.isObject() &&
            document.object().value(QStringLiteral("wabcut")).toInt() == kVersion) {
            for (const QJsonValue& value : document.object().value(QStringLiteral("files")).toArray()) {
                const QUrl file(value.toString(), QUrl::StrictMode);
                if (file.isValid() && file.isLocalFile())
                    files.append(file);
            }
        }
        // "ok" only while the sender waits and this Wabcut can open the files now (its window is up).
        const bool take = !files.isEmpty() && socket->state() == QLocalSocket::ConnectedState && (!m_ready || m_ready());
        socket->setProperty(kStage, take ? Confirm : Done);
        if (take)
            socket->setProperty(kFiles, QVariant::fromValue(files));
        if (socket->state() == QLocalSocket::ConnectedState) {
            socket->write(take ? "ok\n" : "no\n");
            socket->flush();
        }
    }
    if (socket->property(kStage).toInt() != Done && socket->bytesAvailable() > kMaxMessage)
        socket->abort();
}

void bringToFront(QWindow* window) {
    if (!window)
        return;
    const Qt::WindowStates states = window->windowStates();
    if (states & Qt::WindowMinimized)
        window->setWindowStates(states & ~Qt::WindowMinimized); // back to maximized when it was
    if (!window->isVisible())
        window->setVisible(true);
    window->raise();
    window->requestActivate();
}

} // namespace wabcut::app
