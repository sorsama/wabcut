#include "RendererInfo.h"

#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSGRendererInterface>
#include <QThread>
#include <rhi/qrhi.h>

#include <atomic>
#include <mutex>

namespace wabcut::app {

namespace {

// Latest capture in this process (written from render threads, read on the GUI thread).
QMutex g_lastMutex;
QString g_lastApi;
QString g_lastAdapter;

QString apiName(QQuickWindow* window, QRhi* rhi) {
    if (rhi)
        return QString::fromLatin1(rhi->backendName());
    const QSGRendererInterface* ri = window->rendererInterface();
    if (ri && ri->graphicsApi() == QSGRendererInterface::Software)
        return QStringLiteral("Software");
    return {};
}

} // namespace

// One per window attachment. The render thread holds it through its connections; the GUI object severs it
// (target = nullptr) before it detaches or dies, so a capture that races with teardown posts nothing.
struct RendererInfo::Link {
    std::mutex mutex;
    RendererInfo* target = nullptr;
    std::atomic<bool> captured{false};
};

RendererInfo::RendererInfo(QObject* parent) : QObject(parent) {
    QMutexLocker lock(&g_lastMutex);
    m_api = g_lastApi;
    m_adapter = g_lastAdapter;
}

RendererInfo::~RendererInfo() {
    disconnectWindow();
}

void RendererInfo::setWindow(QQuickWindow* window) {
    if (m_window == window)
        return;
    disconnectWindow();
    m_window = window;
    if (m_window) {
        m_link = std::make_shared<Link>();
        m_link->target = this;
        const std::shared_ptr<Link> link = m_link;
        QQuickWindow* w = m_window;
        // Runs where the scene graph signal is emitted: the render thread (threaded render loop) or the GUI
        // thread (basic loop, offscreen grab). The window's QRhi is only valid there and then, so the strings
        // are read here and only the copies leave this function.
        const auto capture = [link, w] {
            if (link->captured.load(std::memory_order_acquire))
                return;
            QRhi* rhi = w->rhi();
            const QString api = apiName(w, rhi);
            if (api.isEmpty())
                return; // not initialized yet; the next frame tries again
            const QString adapter = rhi ? QString::fromUtf8(rhi->driverInfo().deviceName).trimmed() : QString();
            link->captured.store(true, std::memory_order_release);
            {
                QMutexLocker lock(&g_lastMutex);
                g_lastApi = api;
                g_lastAdapter = adapter;
            }
            RendererInfo* target = nullptr;
            {
                std::lock_guard lock(link->mutex);
                target = link->target;
                if (target && target->thread() != QThread::currentThread()) {
                    // Render thread: queue the copies to the GUI object. Posting under the lock means the target
                    // cannot be destroyed in between; a queued call to a destroyed object is dropped by Qt.
                    QMetaObject::invokeMethod(
                        target, [target, api, adapter] { target->apply(api, adapter); }, Qt::QueuedConnection);
                    return;
                }
            }
            if (target)
                target->apply(api, adapter); // same thread (offscreen grab): no queue needed
        };
        m_connections = {
            connect(w, &QQuickWindow::sceneGraphInitialized, w, capture, Qt::DirectConnection),
            connect(w, &QQuickWindow::beforeRendering, w, capture, Qt::DirectConnection),
            // A re-initialized scene graph (e.g. after device loss) may use another adapter: capture again.
            connect(w, &QQuickWindow::sceneGraphInvalidated, w,
                    [link] { link->captured.store(false, std::memory_order_release); }, Qt::DirectConnection),
        };
    }
    emit windowChanged();
}

void RendererInfo::disconnectWindow() {
    for (const QMetaObject::Connection& c : m_connections)
        disconnect(c);
    m_connections.clear();
    if (m_link) {
        std::lock_guard lock(m_link->mutex);
        m_link->target = nullptr;
    }
    m_link.reset();
}

void RendererInfo::apply(const QString& api, const QString& adapter) {
    if (api == m_api && adapter == m_adapter)
        return;
    m_api = api;
    m_adapter = adapter;
    emit infoChanged();
}

QString RendererInfo::summary() const {
    const QString shortName = shortAdapter();
    if (shortName.isEmpty())
        return m_api;
    return m_api + QStringLiteral(" · ") + shortName;
}

QString RendererInfo::shortAdapterName(const QString& deviceName) {
    QString name = deviceName;
    static const QRegularExpression trademarks(QStringLiteral("\\((R|TM|C)\\)"), QRegularExpression::CaseInsensitiveOption);
    name.remove(trademarks);
    static const QRegularExpression noise(QStringLiteral("\\b(NVIDIA|GeForce|AMD|ATI|Intel|Corporation|Graphics Adapter)\\b"),
                                          QRegularExpression::CaseInsensitiveOption);
    const QString stripped = name;
    name.remove(noise);
    name = name.simplified();
    // Keep the original when stripping would leave nothing useful (e.g. "Intel Graphics").
    return name.isEmpty() ? stripped.simplified() : name;
}

} // namespace wabcut::app
