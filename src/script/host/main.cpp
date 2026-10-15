// wabcut-scripthost: one extension's JavaScript, out of the editor's process (ScriptHost.h, docs/extensions.md).
//   wabcut-scripthost --extension <id>
// The editor starts it, sends the hello on stdin and reads the answers on stdout; the id only names the process.

#include "ScriptHost.h"

#include <wabcut/script/Protocol.h>

#include <QCoreApplication>
#include <QMetaObject>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

using wabcut::script::Json;
using wabcut::script::host::Output;
using wabcut::script::host::Runtime;

/// The editor's hello carries the scripts (at most 4 MiB, escaped); nothing it sends is longer.
constexpr std::size_t kMaxInputLine = std::size_t{32} * 1024 * 1024;

/// Every event the main thread handles runs as one watched section (Runtime::enterSection): JavaScript reached
/// through any event, promise reactions included, is under the time limit.
class GuardedApplication : public QCoreApplication {
public:
    GuardedApplication(int& argc, char** argv) : QCoreApplication(argc, argv) {}
    void setRuntime(Runtime* runtime) { m_runtime = runtime; }

    bool notify(QObject* receiver, QEvent* event) override {
        if (!m_runtime) {
            return QCoreApplication::notify(receiver, event);
        }
        m_runtime->enterSection();
        const bool handled = QCoreApplication::notify(receiver, event);
        m_runtime->leaveSection();
        return handled;
    }

private:
    Runtime* m_runtime = nullptr;
};

void route(const std::string& line, Runtime& runtime, Output& output, QCoreApplication& app) {
    Json message = Json::parse(line, nullptr, false);
    if (!message.is_object() || !message.contains("type") || !message.at("type").is_string()) {
        std::fprintf(stderr, "wabcut-scripthost: ignored a line that is not a message\n");
        return;
    }
    const std::string type = message.at("type").get<std::string>();
    if (type == "reply") {
        const auto id = message.value("id", std::uint64_t{0}); // read before the message is moved away
        runtime.deliverReply(id, std::move(message));
    } else if (type == "cancel") {
        const auto call = message.value("call", std::uint64_t{0});
        runtime.cancel(call); // interrupts the script if it is running this call
        QMetaObject::invokeMethod(&app, [&runtime, call] { runtime.handle(Json{{"type", "cancelled"}, {"call", call}}); },
                                  Qt::QueuedConnection);
    } else if (type == "ping") {
        output.write(Json{{"type", "pong"}, {"id", message.value("id", std::uint64_t{0})}});
    } else if (type == "shutdown") {
        runtime.editorGone();
        QMetaObject::invokeMethod(&app, &QCoreApplication::quit, Qt::QueuedConnection);
    } else {
        QMetaObject::invokeMethod(&app, [&runtime, m = std::move(message)] { runtime.handle(m); }, Qt::QueuedConnection);
    }
}

void readInput(Runtime& runtime, Output& output, QCoreApplication& app) {
    std::string pending;
    char buffer[65536];
    for (;;) {
        // A plain read returns what the pipe holds (fread would wait for a full buffer).
#ifdef _WIN32
        const int got = _read(_fileno(stdin), buffer, static_cast<unsigned>(sizeof buffer));
#else
        const auto got = ::read(STDIN_FILENO, buffer, sizeof buffer);
#endif
        if (got <= 0) {
            break; // the editor closed the pipe
        }
        pending.append(buffer, static_cast<std::size_t>(got));
        std::size_t start = 0;
        for (std::size_t nl = pending.find('\n'); nl != std::string::npos; nl = pending.find('\n', start)) {
            std::string line = pending.substr(start, nl - start);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            try {
                route(line, runtime, output, app);
            } catch (const Json::exception& e) {
                std::fprintf(stderr, "wabcut-scripthost: ignored a malformed message: %s\n", e.what());
            }
            start = nl + 1;
        }
        pending.erase(0, start);
        if (pending.size() > kMaxInputLine) {
            std::fprintf(stderr, "wabcut-scripthost: an input line is too long\n");
            break;
        }
    }
    runtime.editorGone();
    QMetaObject::invokeMethod(&app, &QCoreApplication::quit, Qt::QueuedConnection);
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    // A crash ends the process quietly (the editor reports it); no error dialog waits for a click.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
    GuardedApplication app(argc, argv);
    Output output;
    int code = 0;
    {
        Runtime runtime(output);
        app.setRuntime(&runtime);
        std::thread reader([&] { readInput(runtime, output, app); });
        reader.detach(); // it may sit in a read of stdin; the process ends below without waiting for it
        code = QCoreApplication::exec();
        std::fflush(stdout);
        // Leave without tearing the engine down under a reader thread that still holds references to it.
        std::_Exit(code);
    }
}
