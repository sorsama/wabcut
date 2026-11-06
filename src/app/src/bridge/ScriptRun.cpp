#include "ScriptRun.h"

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/command/mcp/McpServer.h>
#include <wabcut/command/mcp/McpTransport.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRandomGenerator>
#include <QThread>
#include <QTimer>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <optional>

namespace wabcut::app {

namespace {

using Json = nlohmann::json;

constexpr int kMaxMessageBytes = 4 * 1024 * 1024; // McpServerOptions::maxMessageBytes
constexpr int kHelloMilliseconds = 10'000;
constexpr int kMaxBadAttempts = 3;
constexpr int kMaxLineChars = 4000;

QByteArray randomHex(int bytes) {
    QByteArray raw;
    while (raw.size() < bytes) {
        const quint32 word = QRandomGenerator::system()->generate();
        raw.append(reinterpret_cast<const char*>(&word), static_cast<qsizetype>(sizeof word));
    }
    raw.truncate(bytes);
    return raw.toHex();
}

/// Same length, same time: a token comparison that does not stop at the first difference.
bool sameToken(const QByteArray& a, const QByteArray& b) {
    if (a.size() != b.size()) {
        return false;
    }
    unsigned char difference = 0;
    for (qsizetype i = 0; i < a.size(); ++i) {
        difference = static_cast<unsigned char>(difference | (static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i])));
    }
    return difference == 0;
}

bool looksLikeCredential(const QString& name) {
    const QString upper = name.toUpper();
    for (const char* word : {"KEY", "TOKEN", "SECRET", "PASSWORD", "PASSWD", "CREDENTIAL"}) {
        if (upper.contains(QLatin1String(word))) {
            return true;
        }
    }
    return false;
}

} // namespace

// ---- The worker ----------------------------------------------------------------------------------------------------

/// The MCP server's view of the socket: lines in (read off the socket as they arrive, also while a long call runs, so a
/// cancellation reaches it) and lines out.
class ScriptServer::Transport : public command::mcp::McpTransport {
public:
    explicit Transport(ScriptServer& owner) : m_owner(owner) {}

    std::optional<std::string> read() override { return poll(); }

    std::optional<std::string> poll() override {
        if (m_owner.m_cancel.load() && !m_owner.m_currentRequest.empty() && m_cancelledRequest != m_owner.m_currentRequest) {
            m_cancelledRequest = m_owner.m_currentRequest;
            return Json{{"jsonrpc", "2.0"},
                        {"method", "notifications/cancelled"},
                        {"params", Json{{"requestId", Json::parse(m_owner.m_currentRequest)}}}}
                .dump();
        }
        if (m_owner.m_socket) {
            m_owner.m_socket->waitForReadyRead(0); // lets the pipe deliver what has arrived
            m_owner.m_buffer += m_owner.m_socket->readAll();
        }
        QByteArray line;
        if (m_owner.takeLine(line)) {
            return line.toStdString();
        }
        return std::nullopt;
    }

    void write(std::string_view line) override {
        if (!m_owner.m_socket) {
            return;
        }
        QByteArray data(line.data(), static_cast<qsizetype>(line.size()));
        data.append('\n');
        m_owner.m_socket->write(data);
        m_owner.m_socket->flush();
    }

private:
    ScriptServer& m_owner;
    std::string m_cancelledRequest;
};

ScriptServer::ScriptServer(Config config) : m_config(std::move(config)) {}

ScriptServer::~ScriptServer() = default;

ProjectSnapshot ScriptServer::result() const {
    const std::lock_guard lock(m_resultMutex);
    return m_result;
}

void ScriptServer::start() {
    using namespace command::mcp;
    m_session = std::make_unique<EditSession>(*m_config.base);
    m_initial = m_session->snapshot();
    McpServerOptions options;
    options.allowed = m_config.allowed;
    options.fileRoots = m_config.fileRoots;
    options.services = m_config.services;
    options.holdModelCommands = true; // the "Analyse media and use AI features" switch gates the model commands too
    options.transactions = true; // `with session.transaction():` in the script
    options.callTimeout = m_config.callTimeout;
    options.serverVersion = m_config.appVersion;
    options.log = [](std::string_view) {};
    if (!m_config.auditFile.isEmpty()) {
        QDir().mkpath(QFileInfo(m_config.auditFile).absolutePath());
        if (QFileInfo(m_config.auditFile).size() > 2 * 1024 * 1024) { // keep one old file
            QFile::remove(m_config.auditFile + QStringLiteral(".1"));
            QFile::rename(m_config.auditFile, m_config.auditFile + QStringLiteral(".1"));
        }
        writeAudit(QStringLiteral("start"), {}, true, {}, false, false, 0);
    }
    options.audit = [this](const McpAuditEntry& entry) {
        writeAudit(QStringLiteral("call"), QString::fromStdString(entry.tool), entry.ok, QString::fromStdString(entry.code),
                   entry.committed, entry.dryRun, entry.revision);
        if (entry.tool == "edit_transaction") {
            return; // its inner edits are counted; the transaction itself is not an edit
        }
        ++m_commands;
        if (entry.committed) {
            ++m_edits;
        }
        emit activity(m_commands.load(), m_edits.load());
    };
    // No project file: saving is the user's (File > Save), never a script's.
    m_mcp = std::make_unique<McpServer>(*m_session, std::move(options), ai::edit::editingCommands());
    m_transport = std::make_unique<Transport>(*this);

    m_helloTimer = new QTimer(this);
    m_helloTimer->setSingleShot(true);
    connect(m_helloTimer, &QTimer::timeout, this, [this] { drop(QStringLiteral("no token within 10 seconds")); });

    m_server = new QLocalServer(this);
    m_server->setSocketOptions(QLocalServer::UserAccessOption); // only this user can open it
    m_server->setMaxPendingConnections(2);
    QLocalServer::removeServer(m_config.address);               // a stale socket file after a crash (Unix)
    if (!m_server->listen(m_config.address)) {
        emit failed(tr("Wabcut could not open the local connection for the script: %1").arg(m_server->errorString()));
        return;
    }
    connect(m_server, &QLocalServer::newConnection, this, &ScriptServer::onNewConnection);
    emit listening(m_server->fullServerName());
}

void ScriptServer::writeAudit(const QString& event, const QString& tool, bool ok, const QString& code, bool committed,
                              bool dryRun, std::uint64_t revision) const {
    if (m_config.auditFile.isEmpty()) {
        return;
    }
    Json line{{"time", QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toStdString()},
              {"event", event.toStdString()},
              {"script", m_config.label}};
    if (event == QLatin1String("call")) {
        line["tool"] = tool.toStdString();
        line["ok"] = ok;
        line["code"] = code.toStdString();
        line["committed"] = committed;
        line["dryRun"] = dryRun;
        line["revision"] = revision;
    }
    QFile file(m_config.auditFile);
    if (file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        file.write(QByteArray::fromStdString(line.dump()) + '\n');
    }
}

void ScriptServer::onNewConnection() {
    while (QLocalSocket* socket = m_server->nextPendingConnection()) {
        if (m_socket) {
            socket->abort(); // one script, one connection
            socket->deleteLater();
            continue;
        }
        m_socket = socket;
        m_authenticated = false;
        m_buffer.clear();
        connect(socket, &QLocalSocket::readyRead, this, &ScriptServer::onReadyRead);
        connect(socket, &QLocalSocket::disconnected, this, &ScriptServer::onDisconnected);
        m_helloTimer->start(kHelloMilliseconds);
        if (socket->bytesAvailable() > 0) {
            onReadyRead();
        }
    }
}

bool ScriptServer::takeLine(QByteArray& line) {
    const qsizetype newline = m_buffer.indexOf('\n');
    if (newline < 0) {
        return false;
    }
    line = m_buffer.left(newline);
    m_buffer.remove(0, newline + 1);
    if (line.endsWith('\r')) {
        line.chop(1);
    }
    return true;
}

void ScriptServer::onReadyRead() {
    if (!m_socket || m_inHandle) {
        return; // a request is being served: its checkpoints read what arrives
    }
    m_buffer += m_socket->readAll();
    QByteArray line;
    while (m_socket && !m_stopped && takeLine(line)) {
        if (!m_authenticated) {
            hello(line);
            continue;
        }
        if (line.size() > kMaxMessageBytes) {
            drop(QStringLiteral("a message over 4 MiB"));
            return;
        }
        // The id of the request, so a cancellation can name it (the transport's cancel hook).
        m_currentRequest.clear();
        if (const Json message = Json::parse(line.constData(), line.constData() + line.size(), nullptr, false);
            message.is_object()) {
            if (message.value("method", std::string()) == "notifications/wabcut-abort") {
                m_aborted = true; // the script's session ended with an error: its edits are discarded
                continue;
            }
            if (message.contains("id")) {
                m_currentRequest = message["id"].dump();
            }
        }
        m_inHandle = true;
        m_mcp->handle(std::string_view(line.constData(), static_cast<std::size_t>(line.size())), *m_transport);
        m_inHandle = false;
        m_currentRequest.clear();
        if (m_pendingDisconnect) {
            m_pendingDisconnect = false;
            onDisconnected();
            return;
        }
    }
    if (m_socket && m_buffer.size() > kMaxMessageBytes) {
        drop(QStringLiteral("a message over 4 MiB"));
    }
}

void ScriptServer::hello(const QByteArray& line) {
    const Json message = Json::parse(line.constData(), line.constData() + line.size(), nullptr, false);
    const bool valid = message.is_object() && message.value("wabcut", std::string()) == "hello" && message.contains("token") &&
                       message["token"].is_string() &&
                       sameToken(QByteArray::fromStdString(message["token"].get<std::string>()), m_config.token);
    if (!valid) {
        ++m_badAttempts;
        m_socket->write(QByteArray(R"({"ok":false,"error":"the token is wrong"})") + '\n');
        m_socket->flush();
        drop(QStringLiteral("a wrong token"), true); // after what it was told is on its way
        if (m_badAttempts >= kMaxBadAttempts) {
            emit failed(tr("Something else tried to connect to the script's connection with a wrong token; the run was stopped."));
        }
        return;
    }
    m_authenticated = true;
    m_everConnected = true;
    m_helloTimer->stop();
    m_server->close(); // nobody else may join
    const bool edit = m_config.allowed.has(command::Capability::EditSequence) || m_config.allowed.has(command::Capability::EditProject);
    Json answer{{"ok", true},
                      {"edit", edit},
                      {"analysis", m_config.allowed.has(command::Capability::MediaAnalysis)},
                      {"files", !m_config.fileRoots.empty()},
                      {"version", m_config.appVersion},
                      {"label", m_config.label}};
    if (m_config.sequence != 0) {
        answer["sequence"] = m_config.sequence; // what commands work on when a call names no sequence
    }
    m_socket->write(QByteArray::fromStdString(answer.dump()) + '\n');
    m_socket->flush();
    emit connected();
}

void ScriptServer::drop(const QString&, bool graceful) {
    if (m_socket) {
        QLocalSocket* socket = m_socket;
        m_socket = nullptr;
        m_buffer.clear();
        socket->disconnect(this);
        if (graceful) {
            // Let the answer already written reach the client (abort() would drop it), then go.
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            QTimer::singleShot(2000, socket, &QObject::deleteLater);
            socket->disconnectFromServer();
        } else {
            socket->abort();
            socket->deleteLater();
        }
    }
    m_helloTimer->stop();
    if (m_badAttempts >= kMaxBadAttempts) {
        stop();
    }
}

void ScriptServer::onDisconnected() {
    if (m_inHandle) {
        m_pendingDisconnect = true;
        return;
    }
    const bool wasAuthenticated = m_authenticated;
    QLocalSocket* socket = m_socket;
    m_socket = nullptr;
    if (socket) {
        socket->disconnect(this);
        socket->deleteLater();
    }
    m_helloTimer->stop();
    m_authenticated = false;
    if (wasAuthenticated) {
        stop(); // the script is done with the project
    }
}

void ScriptServer::stop() {
    if (m_stopped) {
        return;
    }
    m_stopped = true;
    if (m_server) {
        m_server->close();
    }
    if (m_socket) {
        QLocalSocket* socket = m_socket;
        m_socket = nullptr;
        socket->disconnect(this);
        socket->abort();
        socket->deleteLater();
    }
    if (m_mcp) {
        m_mcp->finish(); // an open transaction is rolled back
    }
    if (m_session) {
        const std::lock_guard lock(m_resultMutex);
        m_result = m_session->snapshot();
        m_changed = m_result.get() != m_initial.get(); // an undone or rolled back edit restores the very same snapshot
    }
    writeAudit(QStringLiteral("end"), {}, true, {}, false, false, 0);
    emit stopped();
}

// ---- The run -------------------------------------------------------------------------------------------------------

ScriptRun::ScriptRun(ScriptRunOptions options, OutputHandler output, FinishedHandler finished, ProgressHandler progress,
                     QObject* parent)
    : QObject(parent), m_options(std::move(options)), m_output(std::move(output)), m_finished(std::move(finished)),
      m_progress(std::move(progress)) {}

ScriptRun::~ScriptRun() {
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->waitForFinished(2000);
    }
    if (m_server) {
        m_server->requestCancel();
    }
    if (m_thread) {
        m_thread->quit();
        m_thread->wait(5000);
    }
}

void ScriptRun::start() {
    if (m_state != State::Idle) {
        return;
    }
    m_state = State::Starting;
    ScriptServer::Config config;
    config.base = m_options.base;
    config.allowed = m_options.allowed;
    config.fileRoots = m_options.fileRoots;
    config.services = m_options.services;
    config.token = randomHex(16);
    config.address = QStringLiteral("wabcut-s-") + QString::fromLatin1(randomHex(8));
    config.appVersion = m_options.appVersion.toStdString();
    config.label = m_options.name.toStdString();
    config.sequence = m_options.sequence;
    config.auditFile = m_options.auditFile;
    config.callTimeout = m_options.callTimeout;
    m_token = config.token;

    m_thread = new QThread(this);
    m_thread->setObjectName(QStringLiteral("wabcut-script-run"));
    auto* server = new ScriptServer(std::move(config));
    server->moveToThread(m_thread);
    m_server = server;
    connect(m_thread, &QThread::finished, server, &QObject::deleteLater);
    connect(server, &ScriptServer::listening, this, &ScriptRun::onListening);
    connect(server, &ScriptServer::failed, this, [this](const QString& message) { fail(message); });
    connect(server, &ScriptServer::activity, this, [this](int commands, int edits) {
        if (m_progress) {
            m_progress(commands, edits);
        }
    });
    connect(server, &ScriptServer::stopped, this, &ScriptRun::onServerStopped);
    m_thread->start();
    QMetaObject::invokeMethod(server, &ScriptServer::start, Qt::QueuedConnection);
}

void ScriptRun::onListening(const QString& address) {
    if (m_state != State::Starting) {
        return;
    }
    m_process = new QProcess(this);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for (const QString& key : env.keys()) {
        // Wabcut's own provider keys (set as WABCUT_... variables) never reach a script.
        if (key.startsWith(QLatin1String("WABCUT_"), Qt::CaseInsensitive) && looksLikeCredential(key)) {
            env.remove(key);
        }
    }
    env.insert(QStringLiteral("WABCUT_SESSION"), address);
    env.insert(QStringLiteral("WABCUT_TOKEN"), QString::fromLatin1(m_token));
    env.insert(QStringLiteral("WABCUT_SCRIPT"), QDir::toNativeSeparators(m_options.scriptPath));
    // wabcut-cli opens other projects with its own rights (edit, analysis, any folder), outside this run's grants: it is
    // offered to a script only when the person allowed commands that read or write files.
    if (!m_options.cliPath.isEmpty() && !m_options.fileRoots.empty()) {
        env.insert(QStringLiteral("WABCUT_CLI"), QDir::toNativeSeparators(m_options.cliPath));
    }
    if (!m_options.packageFolder.isEmpty()) {
        const QString folder = QDir::toNativeSeparators(m_options.packageFolder);
        const QString existing = env.value(QStringLiteral("PYTHONPATH"));
        env.insert(QStringLiteral("PYTHONPATH"), existing.isEmpty() ? folder : folder + QDir::listSeparator() + existing);
    }
    env.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
    env.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    env.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
    m_process->setProcessEnvironment(env);
    m_process->setWorkingDirectory(m_options.workingFolder.isEmpty() ? QFileInfo(m_options.scriptPath).absolutePath()
                                                                     : m_options.workingFolder);
    m_process->setProgram(m_options.python.program);
    QStringList arguments = m_options.python.arguments;
    arguments << QStringLiteral("-u") << QStringLiteral("-X") << QStringLiteral("utf8") << QDir::toNativeSeparators(m_options.scriptPath);
    m_process->setArguments(arguments);
    connect(m_process, &QProcess::readyReadStandardOutput, this, [this] { drainOutput(false); });
    connect(m_process, &QProcess::readyReadStandardError, this, [this] { drainOutput(true); });
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            fail(tr("Python could not be started: %1").arg(m_process->errorString()));
        }
    });
    connect(m_process, &QProcess::finished, this,
            [this](int code, QProcess::ExitStatus status) { onProcessFinished(code, static_cast<int>(status)); });
    m_state = State::Running;
    if (m_output) {
        m_output(QStringLiteral("info"), tr("Running %1 with %2").arg(m_options.name, m_options.python.display()));
    }
    m_process->start();
}

void ScriptRun::drainOutput(bool error) {
    if (!m_process) {
        return;
    }
    QByteArray& buffer = error ? m_errBuffer : m_outBuffer;
    buffer += error ? m_process->readAllStandardError() : m_process->readAllStandardOutput();
    for (;;) {
        const qsizetype newline = buffer.indexOf('\n');
        if (newline < 0) {
            break;
        }
        QString line = QString::fromUtf8(buffer.left(newline)).trimmed();
        buffer.remove(0, newline + 1);
        if (line.size() > kMaxLineChars) {
            line = line.left(kMaxLineChars) + QStringLiteral("…");
        }
        if (m_output) {
            m_output(error ? QStringLiteral("err") : QStringLiteral("out"), line);
        }
    }
    if (buffer.size() > 64 * 1024) { // a line that never ends
        const QString text = QString::fromUtf8(buffer.left(kMaxLineChars)) + QStringLiteral("…");
        buffer.clear();
        if (m_output) {
            m_output(error ? QStringLiteral("err") : QStringLiteral("out"), text);
        }
    }
}

void ScriptRun::onProcessFinished(int exitCode, int status) {
    drainOutput(false);
    drainOutput(true);
    for (bool error : {false, true}) { // text without a final newline
        QByteArray& buffer = error ? m_errBuffer : m_outBuffer;
        if (!buffer.trimmed().isEmpty() && m_output) {
            m_output(error ? QStringLiteral("err") : QStringLiteral("out"), QString::fromUtf8(buffer).trimmed().left(kMaxLineChars));
        }
        buffer.clear();
    }
    m_processDone = true;
    m_exitCode = exitCode;
    m_crashed = status == static_cast<int>(QProcess::CrashExit);
    if (m_state == State::Done) {
        return;
    }
    m_state = State::Stopping;
    if (m_server) {
        QMetaObject::invokeMethod(m_server.data(), &ScriptServer::stop, Qt::QueuedConnection);
    }
    maybeFinish();
}

void ScriptRun::onServerStopped() {
    m_serverDone = true;
    maybeFinish();
}

/// Both the script and the worker are done: the outcome.
void ScriptRun::maybeFinish() {
    if (m_state == State::Done || !m_processDone || !m_serverDone) {
        return;
    }
    ScriptOutcome outcome;
    outcome.base = m_options.base;
    outcome.cancelled = m_cancelled;
    outcome.exitCode = m_exitCode;
    outcome.commands = m_server ? m_server->commands() : 0;
    outcome.edits = m_server ? m_server->edits() : 0;
    if (m_cancelled) {
        outcome.message = tr("Stopped. Nothing was changed.");
    } else if (!m_failure.isEmpty()) {
        outcome.message = m_failure;
    } else if (m_crashed) {
        outcome.message = tr("The script was ended unexpectedly. Nothing was changed.");
    } else if (m_server && m_server->aborted()) {
        outcome.message = tr("The script's session ended with an error, so nothing was applied.");
    } else if (m_exitCode != 0) {
        outcome.message = tr("The script failed (exit code %1). Nothing was changed.").arg(m_exitCode);
    } else {
        outcome.ok = true;
        outcome.result = m_server && m_server->changed() ? m_server->result() : m_options.base;
    }
    finish(std::move(outcome));
}

void ScriptRun::fail(const QString& message) {
    if (m_state == State::Done || !m_failure.isEmpty()) {
        return;
    }
    m_failure = message;
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->kill(); // onProcessFinished follows
        return;
    }
    m_processDone = true;
    m_state = State::Stopping;
    if (m_server) {
        QMetaObject::invokeMethod(m_server.data(), &ScriptServer::stop, Qt::QueuedConnection);
    } else {
        m_serverDone = true;
        maybeFinish();
    }
}

void ScriptRun::cancel() {
    if (m_state == State::Idle || m_state == State::Done || m_cancelled) {
        return;
    }
    m_cancelled = true;
    if (m_server) {
        m_server->requestCancel();
    }
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->kill();
        return; // onProcessFinished -> the server stops -> the outcome
    }
    m_processDone = true;
    m_state = State::Stopping;
    if (m_server) {
        QMetaObject::invokeMethod(m_server.data(), &ScriptServer::stop, Qt::QueuedConnection);
    } else {
        m_serverDone = true;
        maybeFinish();
    }
}

void ScriptRun::finish(ScriptOutcome outcome) {
    if (m_state == State::Done) {
        return;
    }
    m_state = State::Done;
    if (m_thread) {
        m_thread->quit();
    }
    if (m_finished) {
        m_finished(outcome);
    }
}

} // namespace wabcut::app
