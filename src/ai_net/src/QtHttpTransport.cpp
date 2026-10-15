#include <wabcut/ai_net/Network.h>
#include <wabcut/model/MediaLocator.h>

#include <QByteArray>
#include <QFile>
#include <QHttpMultiPart>
#include <QHttpPart>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>
#include <system_error>

namespace wabcut::ai::net {

namespace {

/// One exchange, shared by the calling thread (which waits) and the network thread (which runs it).
struct Exchange {
    std::mutex mutex;
    std::condition_variable finished;
    bool done = false;
    std::expected<HttpResponse, AiError> result = std::unexpected(AiError(AiErrorCode::Network, "not started"));
    std::atomic<bool> cancelRequested{false};
    QPointer<QNetworkReply> reply; // network thread only

    /// Completes the exchange exactly once: later calls (an abort racing the end of the reply) are ignored.
    void complete(std::expected<HttpResponse, AiError> r) {
        {
            std::lock_guard lock(mutex);
            if (done) {
                return;
            }
            result = std::move(r);
            done = true;
        }
        finished.notify_all();
    }
};

/// Per-reply state on the network thread.
struct ReplyState {
    bool headDone = false;
    int status = 0;
    HttpHeaders headers;
    std::string body;
    std::optional<StreamDecoder> decoder;
    bool abortedByHandler = false;
    bool timedOut = false;
    bool tooLarge = false;
    std::optional<AiError> failure; // a handler threw, or the stream broke its bounds: the request fails with this
};

/// The catch boundary of everything that runs on the network thread for a reply: an exception from a response
/// handler (a provider sending something malformed) must not escape into Qt's event loop, where it would end the
/// process. It aborts the reply instead; the finished handler then completes the exchange once with the error.
template <class Fn>
void guarded(QNetworkReply* reply, ReplyState& state, Fn&& fn) {
    try {
        fn();
    } catch (const std::exception& e) {
        state.failure = AiError(AiErrorCode::ProviderError, std::string("a response handler failed: ") + e.what());
        reply->abort();
    } catch (...) {
        state.failure = AiError(AiErrorCode::ProviderError, "a response handler failed");
        reply->abort();
    }
}

QString qs(std::string_view s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }

QString path(const std::filesystem::path& p) {
    const auto u8 = p.u8string();
    return QString::fromUtf8(reinterpret_cast<const char*>(u8.data()), static_cast<qsizetype>(u8.size()));
}

AiError networkError(const QString& message) {
    return AiError(AiErrorCode::Network, Redactor::global().redact(message.toStdString()));
}

} // namespace

struct QtHttpTransport::Impl {
    QThread thread;
    QObject* context = nullptr;             // lives on `thread`; owns the access manager
    QNetworkAccessManager* manager = nullptr;

    void start(const std::shared_ptr<Exchange>& exchange, const HttpRequest& request, const HttpStreamHandlers& handlers);
};

void QtHttpTransport::Impl::start(const std::shared_ptr<Exchange>& exchange, const HttpRequest& request,
                                  const HttpStreamHandlers& handlers) {
    if (exchange->cancelRequested) {
        exchange->complete(std::unexpected(AiError(AiErrorCode::Cancelled, "cancelled before sending")));
        return;
    }
    QNetworkRequest qrequest{QUrl(qs(request.url))};
    for (const auto& h : request.headers) {
        qrequest.setRawHeader(QByteArray::fromStdString(h.name), QByteArray::fromStdString(h.value));
    }
    qrequest.setTransferTimeout(request.idleTimeout);
    // Same-origin redirects are followed here; a caller that expects a hop to a CDN follows it itself after checking
    // it (Redirects.h), so it asks for the 3xx answer instead.
    qrequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                          request.followRedirects ? QNetworkRequest::SameOriginRedirectPolicy : QNetworkRequest::ManualRedirectPolicy);
    const QByteArray verb = QByteArray::fromStdString(request.method);

    QNetworkReply* reply = nullptr;
    if (!request.multipart.empty()) {
        auto* multipart = new QHttpMultiPart(QHttpMultiPart::FormDataType);
        for (const auto& field : request.multipart) {
            QHttpPart part;
            QString disposition = QStringLiteral("form-data; name=\"%1\"").arg(qs(field.name));
            if (!field.file.empty()) {
                const std::string fileName = field.fileName.empty() ? pathToUtf8(field.file.filename()) : field.fileName;
                disposition += QStringLiteral("; filename=\"%1\"").arg(qs(fileName));
                auto* file = new QFile(path(field.file), multipart);
                if (!file->open(QIODevice::ReadOnly)) {
                    delete multipart;
                    exchange->complete(std::unexpected(AiError(AiErrorCode::InvalidRequest, "cannot open the upload file")));
                    return;
                }
                part.setBodyDevice(file);
                if (!field.contentType.empty()) {
                    part.setHeader(QNetworkRequest::ContentTypeHeader, qs(field.contentType));
                }
            } else {
                part.setBody(QByteArray::fromStdString(field.value));
            }
            part.setHeader(QNetworkRequest::ContentDispositionHeader, disposition);
            multipart->append(part);
        }
        reply = manager->sendCustomRequest(qrequest, verb, multipart);
        multipart->setParent(reply);
    } else if (!request.bodyFile.empty()) {
        auto* file = new QFile(path(request.bodyFile));
        if (!file->open(QIODevice::ReadOnly)) {
            delete file;
            exchange->complete(std::unexpected(AiError(AiErrorCode::InvalidRequest, "cannot open the upload file")));
            return;
        }
        reply = manager->sendCustomRequest(qrequest, verb, file);
        file->setParent(reply);
    } else if (!request.body.empty()) {
        reply = manager->sendCustomRequest(qrequest, verb, QByteArray::fromStdString(request.body));
    } else {
        reply = manager->sendCustomRequest(qrequest, verb);
    }
    exchange->reply = reply;

    auto state = std::make_shared<ReplyState>();
    const BodyMode mode = request.responseMode;

    auto evaluateHead = [reply, state, mode, &handlers]() -> bool {
        if (state->headDone) {
            return true;
        }
        const QVariant status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        if (!status.isValid()) {
            return true; // no HTTP response yet
        }
        state->headDone = true;
        state->status = status.toInt();
        for (const auto& [name, value] : reply->rawHeaderPairs()) {
            state->headers.push_back({name.toStdString(), value.toStdString()});
        }
        const bool ok = state->status >= 200 && state->status < 300;
        if (!ok) {
            return true; // error bodies are buffered
        }
        if (handlers.onHead && !handlers.onHead(HttpResponseHead{state->status, state->headers})) {
            state->abortedByHandler = true;
            reply->abort();
            return false;
        }
        if (mode != BodyMode::Buffered) {
            state->decoder.emplace(mode, handlers);
        }
        return true;
    };

    auto consume = [reply, state, evaluateHead]() {
        if (!evaluateHead() || state->abortedByHandler || state->failure) {
            return;
        }
        const QByteArray chunk = reply->readAll();
        if (chunk.isEmpty()) {
            return;
        }
        if (state->decoder) {
            if (!state->decoder->feed(std::string_view(chunk.constData(), static_cast<std::size_t>(chunk.size())))) {
                if (state->decoder->error()) {
                    state->failure = *state->decoder->error(); // too large, or malformed
                } else {
                    state->abortedByHandler = true;
                }
                reply->abort();
            }
            return;
        }
        if (static_cast<std::int64_t>(state->body.size()) + chunk.size() > kMaxBufferedBody) {
            state->tooLarge = true;
            reply->abort();
            return;
        }
        state->body.append(chunk.constData(), static_cast<std::size_t>(chunk.size()));
    };

    QObject::connect(reply, &QNetworkReply::readyRead, reply, [reply, state, consume] { guarded(reply, *state, consume); });
    if (handlers.onUploadProgress) {
        QObject::connect(reply, &QNetworkReply::uploadProgress, reply, [reply, state, &handlers](qint64 sent, qint64 total) {
            if (total > 0) {
                guarded(reply, *state, [&] {
                    handlers.onUploadProgress(static_cast<std::uint64_t>(sent), static_cast<std::uint64_t>(total));
                });
            }
        });
    }
    if (request.timeout.count() > 0) {
        QTimer::singleShot(request.timeout, reply, [reply, state] {
            state->timedOut = true;
            reply->abort();
        });
    }
    const auto idle = request.idleTimeout;
    const auto total = request.timeout;
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, state, exchange, consume, idle, total] {
        guarded(reply, *state, consume);
        bool finishedDecoder = true;
        if (!state->failure && !state->abortedByHandler && state->decoder && state->headDone && state->status >= 200 &&
            state->status < 300 && reply->error() == QNetworkReply::NoError) {
            guarded(reply, *state, [&] { finishedDecoder = state->decoder->finish(); });
            if (!finishedDecoder && state->decoder->error() && !state->failure) {
                state->failure = *state->decoder->error();
            }
        }
        std::expected<HttpResponse, AiError> result = std::unexpected(AiError());
        if (state->failure) {
            result = std::unexpected(*state->failure);
        } else if (state->abortedByHandler) {
            result = std::unexpected(AiError(AiErrorCode::Cancelled, "aborted by a response handler"));
        } else if (exchange->cancelRequested) {
            result = std::unexpected(AiError(AiErrorCode::Cancelled, "cancelled"));
        } else if (state->timedOut) {
            result = std::unexpected(AiError(AiErrorCode::Network, "no complete response within " +
                                                                       std::to_string(total.count() / 1000) + " s"));
        } else if (state->tooLarge) {
            result = std::unexpected(AiError(AiErrorCode::ProviderError, "the response is too large"));
        } else if (!state->headDone) {
            AiError e = networkError(reply->errorString());
            if (reply->error() == QNetworkReply::OperationCanceledError || reply->error() == QNetworkReply::TimeoutError) {
                e.message = "no data for " + std::to_string(idle.count() / 1000) + " s";
            }
            result = std::unexpected(std::move(e));
        } else if (state->status >= 200 && state->status < 300 && reply->error() != QNetworkReply::NoError) {
            result = std::unexpected(networkError(reply->errorString())); // the body broke off
        } else if (!finishedDecoder) {
            result = std::unexpected(AiError(AiErrorCode::Cancelled, "aborted by a response handler"));
        } else {
            HttpResponse response;
            response.status = state->status;
            response.headers = std::move(state->headers);
            response.body = std::move(state->body);
            result = std::move(response);
        }
        // Close the upload files now, not at deferred deletion: on Windows an open handle would keep the caller from
        // deleting a temp file (e.g. extracted audio) right after send() returns.
        for (QFile* upload : reply->findChildren<QFile*>()) {
            upload->close();
        }
        reply->deleteLater();
        exchange->complete(std::move(result));
    });
}

QtHttpTransport::QtHttpTransport() : m_impl(std::make_unique<Impl>()) {
    m_impl->thread.setObjectName(QStringLiteral("wabcut-ai-net"));
    m_impl->context = new QObject;
    m_impl->context->moveToThread(&m_impl->thread);
    QObject::connect(&m_impl->thread, &QThread::finished, m_impl->context, &QObject::deleteLater);
    m_impl->thread.start();
    Impl* impl = m_impl.get();
    QMetaObject::invokeMethod(
        m_impl->context,
        [impl] {
            impl->manager = new QNetworkAccessManager(impl->context);
            impl->manager->setRedirectPolicy(QNetworkRequest::SameOriginRedirectPolicy);
        },
        Qt::BlockingQueuedConnection);
}

QtHttpTransport::~QtHttpTransport() {
    m_impl->thread.quit();
    m_impl->thread.wait();
}

std::expected<HttpResponse, AiError> QtHttpTransport::send(const HttpRequest& request, const HttpStreamHandlers& handlers,
                                                           std::stop_token stop) {
    const QUrl url(qs(request.url));
    if (!url.isValid() || (url.scheme() != QLatin1String("https") && url.scheme() != QLatin1String("http"))) {
        return std::unexpected(AiError(AiErrorCode::InvalidRequest, "not an http(s) URL"));
    }
    if (QThread::currentThread() == &m_impl->thread) {
        return std::unexpected(AiError(AiErrorCode::InvalidRequest, "send() called from the network thread"));
    }
    std::error_code ec;
    if (!request.bodyFile.empty() && !std::filesystem::is_regular_file(request.bodyFile, ec)) {
        return std::unexpected(AiError(AiErrorCode::InvalidRequest, "the upload file does not exist"));
    }
    if (stop.stop_requested()) {
        return std::unexpected(AiError(AiErrorCode::Cancelled, "cancelled"));
    }

    auto exchange = std::make_shared<Exchange>();
    Impl* impl = m_impl.get();
    // `request` and `handlers` outlive the exchange: this thread waits below until the network thread is done.
    QMetaObject::invokeMethod(
        impl->context,
        [impl, exchange, &request, &handlers] {
            try {
                impl->start(exchange, request, handlers);
            } catch (const std::exception& e) { // nothing may escape into the network thread's event loop
                exchange->complete(std::unexpected(AiError(AiErrorCode::Network, std::string("could not start the request: ") + e.what())));
            }
        },
        Qt::QueuedConnection);
    std::stop_callback onStop(stop, [impl, exchange] {
        exchange->cancelRequested = true;
        QMetaObject::invokeMethod(
            impl->context,
            [exchange] {
                if (exchange->reply) {
                    exchange->reply->abort();
                }
            },
            Qt::QueuedConnection);
    });
    std::unique_lock lock(exchange->mutex);
    exchange->finished.wait(lock, [&] { return exchange->done; });
    return std::move(exchange->result);
}

NetworkStack::NetworkStack(RequestLog& log, RetryPolicy policy, const Redactor& redactor)
    : m_logging(m_qt, log, redactor), m_retrying(m_logging, policy) {}

} // namespace wabcut::ai::net
