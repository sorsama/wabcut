// QtHttpTransport against an in-process HTTP server on 127.0.0.1 (no external network): SSE streaming, buffered
// bodies, uploads (body file, multipart), HTTP errors with Retry-After through the retrying stack, cancellation,
// idle and total timeouts, handler aborts and the request log.

#include "FakeTransport.h"

#include <wabcut/ai/HttpDownload.h>
#include <wabcut/ai/ModelRegistry.h>
#include <wabcut/ai/Redirects.h>
#include <wabcut/ai_net/Adapters.h>
#include <wabcut/ai_net/Network.h>

#include <QCoreApplication>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <thread>

using namespace wabcut::ai;
using namespace std::chrono_literals;

namespace {

void ensureCoreApp() {
    if (!QCoreApplication::instance()) {
        static int argc = 1;
        static char name[] = "wabcut_ai_net_tests";
        static char* argv[] = {name, nullptr};
        static QCoreApplication app(argc, argv);
    }
}

/// A tiny HTTP/1.1 server on its own thread. Every response closes the connection.
class LocalServer {
public:
    struct Request {
        QByteArray method;
        QByteArray path;
        QByteArray head;
        QByteArray body;
    };
    using Handler = std::function<void(QTcpSocket*, const Request&)>;

    LocalServer() {
        m_context.moveToThread(&m_thread);
        m_thread.start();
        QMetaObject::invokeMethod(
            &m_context,
            [this] {
                m_server = new QTcpServer;
                m_server->listen(QHostAddress::LocalHost, 0);
                m_port = m_server->serverPort();
                QObject::connect(m_server, &QTcpServer::newConnection, m_server, [this] {
                    while (QTcpSocket* socket = m_server->nextPendingConnection()) {
                        auto buffer = std::make_shared<QByteArray>();
                        QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer] { onData(socket, *buffer); });
                        QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                    }
                });
            },
            Qt::BlockingQueuedConnection);
    }
    ~LocalServer() {
        QMetaObject::invokeMethod(&m_context, [this] { delete m_server; }, Qt::BlockingQueuedConnection);
        m_thread.quit();
        m_thread.wait();
    }

    void route(const QByteArray& path, Handler handler) { m_routes[path] = std::move(handler); }
    std::string url(const char* path) const { return "http://127.0.0.1:" + std::to_string(m_port) + path; }
    Request last(const QByteArray& path) const {
        std::lock_guard lock(m_mutex);
        return m_last.at(path);
    }

    static void respond(QTcpSocket* socket, int status, const QByteArray& reason, const QByteArray& headers,
                        const QByteArray& body) {
        socket->write("HTTP/1.1 " + QByteArray::number(status) + " " + reason + "\r\n" + headers +
                      "Content-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        socket->disconnectFromHost();
    }

private:
    void onData(QTcpSocket* socket, QByteArray& buffer) {
        buffer += socket->readAll();
        const auto headEnd = buffer.indexOf("\r\n\r\n");
        if (headEnd < 0) {
            return;
        }
        Request request;
        request.head = buffer.left(headEnd);
        const auto firstLine = request.head.left(request.head.indexOf("\r\n")).split(' ');
        request.method = firstLine.value(0);
        request.path = firstLine.value(1);
        qsizetype length = 0;
        for (const QByteArray& line : request.head.split('\n')) {
            if (line.toLower().startsWith("content-length:")) {
                length = line.mid(15).trimmed().toLongLong();
            }
        }
        if (buffer.size() < headEnd + 4 + length) {
            return; // body still arriving
        }
        request.body = buffer.mid(headEnd + 4, length);
        buffer.clear();
        const QByteArray route = request.path.left(request.path.indexOf('?') < 0 ? request.path.size() : request.path.indexOf('?'));
        {
            std::lock_guard lock(m_mutex);
            m_last[route] = request;
        }
        auto it = m_routes.find(route);
        if (it == m_routes.end()) {
            respond(socket, 404, "Not Found", "", "{}");
            return;
        }
        it->second(socket, request);
    }

    QThread m_thread;
    QObject m_context;
    QTcpServer* m_server = nullptr;
    quint16 m_port = 0;
    std::map<QByteArray, Handler> m_routes;
    mutable std::mutex m_mutex;
    std::map<QByteArray, Request> m_last;
};

/// Streams `events` with `gap` between them, then closes (unless `keepOpen`).
void streamSse(QTcpSocket* socket, QList<QByteArray> events, std::chrono::milliseconds gap, bool keepOpen = false) {
    socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n");
    socket->flush();
    auto next = std::make_shared<std::function<void(int)>>();
    *next = [socket, events, gap, keepOpen, next](int i) {
        if (i == events.size()) {
            if (!keepOpen) {
                socket->disconnectFromHost();
            }
            return;
        }
        socket->write(events[i]);
        socket->flush();
        QTimer::singleShot(gap, socket, [next, i] { (*next)(i + 1); });
    };
    (*next)(0);
}

struct Fixture {
    Fixture() { ensureCoreApp(); }
};

} // namespace

TEST_CASE("QtHttpTransport streams SSE and buffers JSON over a real socket", "[ai][net][qt]") {
    Fixture f;
    LocalServer server;
    server.route("/sse", [](QTcpSocket* s, const LocalServer::Request&) {
        streamSse(s, {"event: one\ndata: {\"n\":1}\n\n", ": keep-alive\n\n", "data: {\"n\":2}\n", "\ndata: [DONE]\n\n"}, 20ms);
    });
    server.route("/echo", [](QTcpSocket* s, const LocalServer::Request& r) {
        const bool auth = r.head.contains("authorization: Bearer secret-key-0123456789") ||
                          r.head.contains("Authorization: Bearer secret-key-0123456789");
        LocalServer::respond(s, 200, "OK", "Content-Type: application/json\r\n",
                             "{\"method\":\"" + r.method + "\",\"auth\":" + (auth ? "true" : "false") +
                                 ",\"bodyLength\":" + QByteArray::number(r.body.size()) + "}");
    });
    net::QtHttpTransport transport;

    HttpRequest sse;
    sse.method = "GET";
    sse.url = server.url("/sse");
    sse.responseMode = BodyMode::Sse;
    std::vector<SseEvent> events;
    int heads = 0;
    HttpStreamHandlers handlers;
    handlers.onHead = [&](const HttpResponseHead& head) {
        ++heads;
        return head.status == 200;
    };
    handlers.onEvent = [&](const SseEvent& e) {
        events.push_back(e);
        return true;
    };
    auto streamed = transport.send(sse, handlers, {});
    REQUIRE(streamed);
    CHECK(streamed->status == 200);
    CHECK(streamed->body.empty());
    CHECK(heads == 1);
    REQUIRE(events.size() == 3);
    CHECK(events[0] == SseEvent{"one", "{\"n\":1}", ""});
    CHECK(events[1].data == "{\"n\":2}");
    CHECK(events[2].data == "[DONE]");

    HttpRequest post;
    post.url = server.url("/echo");
    post.headers = {{"Authorization", "Bearer secret-key-0123456789"}, {"content-type", "application/json"}};
    post.body = R"({"hello":"world"})";
    auto echoed = transport.send(post, {}, {});
    REQUIRE(echoed);
    CHECK(echoed->status == 200);
    const Json body = Json::parse(echoed->body);
    CHECK(body["method"] == "POST");
    CHECK(body["auth"] == true);
    CHECK(body["bodyLength"] == 17);
    CHECK(findHeader(echoed->headers, "content-type") == "application/json");
}

TEST_CASE("QtHttpTransport uploads files and multipart forms", "[ai][net][qt]") {
    Fixture f;
    LocalServer server;
    server.route("/upload", [](QTcpSocket* s, const LocalServer::Request& r) {
        LocalServer::respond(s, 200, "OK", "Content-Type: application/json\r\n",
                             "{\"bodyLength\":" + QByteArray::number(r.body.size()) + ",\"hasFile\":" +
                                 (r.body.contains("FILE-CONTENT-123") ? "true" : "false") + ",\"hasName\":" +
                                 (r.body.contains("filename=\"clip.flac\"") ? "true" : "false") + ",\"hasField\":" +
                                 (r.body.contains("scribe_v2") ? "true" : "false") + "}");
    });
    const auto file = test::uniqueTempPath("wabcut-qt-transport-upload", ".flac");
    {
        std::ofstream out(file, std::ios::binary);
        out << std::string(100'000, 'x') << "FILE-CONTENT-123";
    }
    net::QtHttpTransport transport;

    HttpRequest raw;
    raw.url = server.url("/upload");
    raw.bodyFile = file;
    std::uint64_t lastSent = 0;
    HttpStreamHandlers progress;
    progress.onUploadProgress = [&](std::uint64_t sent, std::uint64_t) { lastSent = sent; };
    auto r1 = transport.send(raw, progress, {});
    REQUIRE(r1);
    CHECK(Json::parse(r1->body)["bodyLength"] == 100'016);
    CHECK(lastSent == 100'016);

    HttpRequest form;
    form.url = server.url("/upload");
    form.multipart = {{"model_id", "scribe_v2", {}, {}, {}}, {"file", {}, file, "clip.flac", "audio/flac"}};
    auto r2 = transport.send(form, {}, {});
    REQUIRE(r2);
    const Json j = Json::parse(r2->body);
    CHECK(j["hasFile"] == true);
    CHECK(j["hasName"] == true);
    CHECK(j["hasField"] == true);

    HttpRequest missing = raw;
    missing.bodyFile = file.parent_path() / "no-such-file.flac";
    CHECK(transport.send(missing, {}, {}).error().code == AiErrorCode::InvalidRequest);

    // The upload file is closed when send() returns, so a caller can delete it at once (Windows refuses to delete an
    // open file). Raw and multipart uploads, repeated to catch a deferred close.
    for (int i = 0; i < 4; ++i) {
        {
            std::ofstream out(file, std::ios::binary);
            out << "FILE-CONTENT-123";
        }
        REQUIRE(transport.send(i % 2 == 0 ? raw : form, {}, {}));
        std::error_code ec;
        CHECK(std::filesystem::remove(file, ec));
        CHECK_FALSE(ec);
    }
}

TEST_CASE("QtHttpTransport through NetworkStack: 429 Retry-After retried, every attempt logged and redacted", "[ai][net][qt]") {
    Fixture f;
    LocalServer server;
    std::atomic<int> calls{0};
    server.route("/v1/messages", [&](QTcpSocket* s, const LocalServer::Request&) {
        if (calls++ == 0) {
            LocalServer::respond(s, 429, "Too Many Requests", "Retry-After: 0\r\nContent-Type: application/json\r\n",
                                 R"({"type":"error","error":{"type":"rate_limit_error","message":"slow down"}})");
        } else {
            LocalServer::respond(s, 200, "OK", "Content-Type: application/json\r\n", R"({"ok":true})");
        }
    });
    RequestLog log;
    net::NetworkStack stack(log);
    HttpRequest request;
    request.url = server.url("/v1/messages?key=SHOULD-NOT-BE-LOGGED");
    request.headers = {{"x-api-key", "sk-ant-local-test-0123456789"}};
    request.body = "{}";
    auto r = stack.transport().send(request, {}, {});
    REQUIRE(r);
    CHECK(r->status == 200);
    CHECK(calls == 2);
    const auto entries = log.entries();
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].status == 429);
    CHECK(entries[0].error == "RateLimited");
    CHECK(entries[1].status == 200);
    for (const auto& e : entries) {
        const std::string dumped = e.toJson().dump();
        CHECK(dumped.find("SHOULD-NOT-BE-LOGGED") == std::string::npos);
        CHECK(dumped.find("sk-ant-local") == std::string::npos);
    }
}

TEST_CASE("QtHttpTransport: cancellation, idle and total timeouts, handler aborts, bad URLs", "[ai][net][qt]") {
    Fixture f;
    LocalServer server;
    server.route("/hang", [](QTcpSocket* s, const LocalServer::Request&) {
        streamSse(s, {"data: {\"first\":true}\n\n"}, 10ms, /*keepOpen=*/true);
    });
    server.route("/slow", [](QTcpSocket* s, const LocalServer::Request&) {
        QList<QByteArray> events;
        for (int i = 0; i < 50; ++i) {
            events.push_back("data: {}\n\n");
        }
        streamSse(s, events, 50ms);
    });
    net::QtHttpTransport transport;

    SECTION("stop token") {
        HttpRequest request;
        request.method = "GET";
        request.url = server.url("/hang");
        request.responseMode = BodyMode::Sse;
        std::stop_source source;
        std::jthread canceller; // joined before `source` goes away
        HttpStreamHandlers h;
        h.onEvent = [&](const SseEvent&) {
            canceller = std::jthread([&source] {
                std::this_thread::sleep_for(50ms);
                source.request_stop();
            });
            return true;
        };
        const auto start = std::chrono::steady_clock::now();
        auto r = transport.send(request, h, source.get_token());
        REQUIRE_FALSE(r);
        CHECK(r.error().code == AiErrorCode::Cancelled);
        CHECK(std::chrono::steady_clock::now() - start < 5s);
    }
    SECTION("idle timeout") {
        HttpRequest request;
        request.method = "GET";
        request.url = server.url("/hang");
        request.responseMode = BodyMode::Sse;
        request.idleTimeout = 300ms;
        int events = 0;
        HttpStreamHandlers h;
        h.onEvent = [&](const SseEvent&) { return ++events > 0; };
        auto r = transport.send(request, h, {});
        REQUIRE_FALSE(r);
        CHECK(r.error().code == AiErrorCode::Network);
        CHECK(events == 1);
    }
    SECTION("total timeout") {
        HttpRequest request;
        request.method = "GET";
        request.url = server.url("/slow");
        request.responseMode = BodyMode::Sse;
        request.timeout = 300ms;
        auto r = transport.send(request, {}, {});
        REQUIRE_FALSE(r);
        CHECK(r.error().code == AiErrorCode::Network);
        CHECK(r.error().message.find("no complete response") != std::string::npos);
    }
    SECTION("a handler stops the stream") {
        HttpRequest request;
        request.method = "GET";
        request.url = server.url("/slow");
        request.responseMode = BodyMode::Sse;
        int events = 0;
        HttpStreamHandlers h;
        h.onEvent = [&](const SseEvent&) { return ++events < 3; };
        auto r = transport.send(request, h, {});
        REQUIRE_FALSE(r);
        CHECK(r.error().code == AiErrorCode::Cancelled);
        CHECK(events == 3);
    }
    SECTION("bad URLs and refused connections") {
        HttpRequest request;
        request.url = "ftp://example.test/x";
        CHECK(transport.send(request, {}, {}).error().code == AiErrorCode::InvalidRequest);
        request.url = "http://127.0.0.1:1/unused";
        CHECK(transport.send(request, {}, {}).error().code == AiErrorCode::Network);
    }
}

TEST_CASE("QtHttpTransport: malformed or endless responses fail the request once; the network thread survives",
          "[ai][net][qt][security]") {
    Fixture f;
    LocalServer server;
    const QList<QByteArray> probe = {"data: {\"id\":null,\"choices\":[]}\n\n", "data: [DONE]\n\n"};
    server.route("/sse", [probe](QTcpSocket* s, const LocalServer::Request&) { streamSse(s, probe, 5ms); });
    server.route("/chat/completions", [probe](QTcpSocket* s, const LocalServer::Request&) { streamSse(s, probe, 5ms); });
    server.route("/endless", [](QTcpSocket* s, const LocalServer::Request&) {
        streamSse(s, {"data: " + QByteArray(3 * 1024 * 1024, 'x')}, 5ms); // a 3 MiB line, never terminated
    });
    server.route("/ok", [](QTcpSocket* s, const LocalServer::Request&) {
        LocalServer::respond(s, 200, "OK", "Content-Type: application/json\r\n", "{\"ok\":true}");
    });
    net::QtHttpTransport transport;

    // A handler doing typed access on the reviewer's probe throws on the network thread: the transport catches it,
    // aborts the reply and completes the request once with ProviderError.
    HttpRequest sse;
    sse.method = "GET";
    sse.url = server.url("/sse");
    sse.responseMode = BodyMode::Sse;
    int calls = 0;
    HttpStreamHandlers throwing;
    throwing.onEvent = [&](const SseEvent& e) {
        ++calls;
        return Json::parse(e.data).value("id", std::string()).empty(); // type_error: "id" is null
    };
    std::expected<HttpResponse, AiError> failed = std::unexpected(AiError());
    REQUIRE_NOTHROW(failed = transport.send(sse, throwing, {}));
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == AiErrorCode::ProviderError);
    CHECK(calls == 1);

    // An endless line is cut off with an explicit error.
    HttpRequest endless = sse;
    endless.url = server.url("/endless");
    HttpStreamHandlers quiet;
    quiet.onEvent = [](const SseEvent&) { return true; };
    const auto cut = transport.send(endless, quiet, {});
    REQUIRE_FALSE(cut);
    CHECK(cut.error().code == AiErrorCode::PayloadTooLarge);

    // The network thread survived: the same transport keeps working.
    HttpRequest ok;
    ok.method = "GET";
    ok.url = server.url("/ok");
    const auto fine = transport.send(ok, {}, {});
    REQUIRE(fine);
    CHECK(fine->status == 200);

    // The real Chat Completions adapter over the real transport with the probe: no crash, a clean result.
    ModelRegistry registry = ModelRegistry::builtin();
    ModelSpec local;
    local.id = "qwen3.5:9b";
    local.provider = "ollama";
    REQUIRE(registry.addModel(local));
    const ProviderSpec& ollama = *registry.provider("ollama");
    auto chat = net::makeChatCompletionsChat(ProviderContext{ollama, *registry.model("qwen3.5:9b"), server.url(""), Secret(), transport});
    ChatRequest request;
    request.messages.push_back(ChatMessage::user("hi"));
    std::expected<ChatResponse, AiError> answer = std::unexpected(AiError());
    REQUIRE_NOTHROW(answer = chat->chat(request, {}, {}));
    CHECK(answer); // {"id": null} is treated as an absent id
}

TEST_CASE("QtHttpTransport: downloads follow an approved cross-origin 302 and leave credentials behind (P1-4)", "[ai][net][qt][redirect]") {
    Fixture f;
    LocalServer origin;
    LocalServer cdn; // another port: another origin, like huggingface.co -> cas-bridge.xethub.hf.co
    cdn.route("/blob", [](QTcpSocket* s, const LocalServer::Request&) {
        LocalServer::respond(s, 200, "OK", "Content-Type: application/octet-stream\r\n", "GGUF-weights-0123456789");
    });
    origin.route("/model.gguf", [&cdn](QTcpSocket* s, const LocalServer::Request&) {
        LocalServer::respond(s, 302, "Found", "Location: " + QByteArray::fromStdString(cdn.url("/blob?X-Amz-Signature=abc")) + "\r\n", "");
    });
    net::QtHttpTransport transport;

    // The transport alone keeps refusing cross-origin redirects (same-origin policy): the download sees the 302 and fails.
    HttpRequest plain;
    plain.method = "GET";
    plain.url = origin.url("/model.gguf");
    plain.responseMode = BodyMode::Raw;
    const auto refusedByQt = transport.send(plain, {}, {});
    CHECK((!refusedByQt || refusedByQt->status == 302));

    // ... and the download path follows the hop on purpose (loopback http needs a test allow-list).
    RedirectAllowList allow;
    allow.httpsOnly = false;
    allow.rules.push_back({"127.0.0.1", {"127.0.0.1"}});
    HttpDownloadTransport downloads(transport, allow);
    std::string received;
    auto response = downloads.get(origin.url("/model.gguf"), 0, {}, [&](std::span<const std::byte> data) {
        received.append(reinterpret_cast<const char*>(data.data()), data.size());
        return true;
    }, {});
    REQUIRE(response);
    CHECK(response->status == 200);
    CHECK(received == "GGUF-weights-0123456789");

    HttpRequest keyed = plain;
    keyed.headers = {{"Authorization", "Bearer secret-key-0123456789"}, {"Range", "bytes=0-"}};
    auto followed = sendFollowingRedirects(transport, keyed, {}, {}, allow);
    REQUIRE(followed);
    const QByteArray head = cdn.last("/blob").head.toLower();
    CHECK_FALSE(head.contains("authorization"));
    CHECK(head.contains("range: bytes=0-"));

    // A hop the list does not allow never reaches the other server.
    LocalServer elsewhere;
    elsewhere.route("/steal", [](QTcpSocket* s, const LocalServer::Request&) { LocalServer::respond(s, 200, "OK", "", "x"); });
    origin.route("/bad", [&elsewhere](QTcpSocket* s, const LocalServer::Request&) {
        LocalServer::respond(s, 302, "Found", "Location: " + QByteArray::fromStdString(elsewhere.url("/steal")).replace("127.0.0.1", "localhost") + "\r\n", "");
    });
    HttpRequest bad = plain;
    bad.url = origin.url("/bad");
    auto refused = sendFollowingRedirects(transport, bad, {}, {}, allow);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().message.find("not an approved download host") != std::string::npos);
    CHECK_THROWS(elsewhere.last("/steal"));
}
