#include "ProgramMonitor.h"

#include "ScopesBridge.h"

#include <wabcut/playback/video/PlaybackController.h>
#include <wabcut/render/CompositeMailbox.h>
#include <wabcut/render/Compositor.h>
#include <wabcut/render/GpuScopes.h>
#include <wabcut/render/ZeroCopy.h>

#include <QCoreApplication>
#include <QQuickWindow>
#include <QScreen>

#include <wabcut/media/VideoDecoder.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <rhi/qrhi.h>

namespace wabcut::app {

namespace {

std::int64_t steadyNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

QString backendLabel(QRhi* rhi) {
    if (!rhi) {
        return {};
    }
    QString api;
    switch (rhi->backend()) {
    case QRhi::D3D11:
        api = QStringLiteral("D3D11");
        break;
    case QRhi::D3D12:
        api = QStringLiteral("D3D12");
        break;
    case QRhi::Vulkan:
        api = QStringLiteral("Vulkan");
        break;
    case QRhi::OpenGLES2:
        api = QStringLiteral("OpenGL");
        break;
    case QRhi::Metal:
        api = QStringLiteral("Metal");
        break;
    case QRhi::Null:
        api = QStringLiteral("Null");
        break;
    }
    const QString adapter = QString::fromUtf8(rhi->driverInfo().deviceName).trimmed();
    return adapter.isEmpty() ? api : api + QStringLiteral(" · ") + adapter;
}

// Zero-copy (docs/zero-copy.md) needs every monitor to draw on the device the decoders decode on. While all the monitors
// render in one window they share its QRhi's device. A monitor in another window (0.1.3: a panel popped out into its own
// window) has a device of its own, which cannot sample the others' decoder surfaces: the decoders then fall back to CPU
// frames, which any device uploads, until the monitors are back in one window. Render threads (one per window) call in.
class DeviceSharing {
public:
    static DeviceSharing& instance() {
        static DeviceSharing sharing;
        return sharing;
    }

    void attach(const void* renderer, QRhi* rhi) {
        std::lock_guard lock(m_mutex);
        if (rhi) {
            m_rhis[renderer] = rhi;
        } else {
            m_rhis.erase(renderer);
        }
        QRhi* only = nullptr;
        bool mixed = false;
        for (const auto& [owner, r] : m_rhis) {
            if (!only) {
                only = r;
            } else if (r != only) {
                mixed = true;
            }
        }
        if (mixed) {
            if (m_shared || !m_mixed) {
                // The decoders opened from now on deliver CPU frames.
                (void)media::setSharedD3D11Device(nullptr, nullptr);
            }
            m_shared = nullptr;
            m_mixed = true;
        } else if (only && (only != m_shared || m_mixed)) {
            (void)render::shareDeviceWithDecoders(only);
            m_shared = only;
            m_mixed = false;
        }
        // No monitor left (windows closing): the decoders keep the device they have.
    }

private:
    std::mutex m_mutex;
    std::map<const void*, QRhi*> m_rhis;
    QRhi* m_shared = nullptr;
    bool m_mixed = false;
};

} // namespace

// ---- Renderer (scene-graph render thread) ----

class ProgramMonitorRenderer final : public QQuickRhiItemRenderer {
public:
    ~ProgramMonitorRenderer() override {
        // Runs on the render thread while the QRhi is still valid (outside a frame: a pending scope readback is
        // waited for).
        m_scopes.reset();
        m_compositor.reset();
        if (m_sharing) {
            DeviceSharing::instance().attach(this, nullptr);
        }
    }

protected:
    void initialize(QRhiCommandBuffer*) override {
        if (m_rhi != rhi()) {
            m_scopes.reset();
            m_compositor.reset(); // a new QRhi (device reset, window move): rebuild everything on it
            m_rhi = rhi();
            // Zero-copy: decoders opened from now on decode on this device (D3D11; elsewhere a no-op) while every
            // monitor draws on it (DeviceSharing). Opt out with WABCUT_ZERO_COPY=0; a second output (P.2) never
            // takes the device from the main monitor.
            if (m_rhi && m_shareDevice && qEnvironmentVariable("WABCUT_ZERO_COPY") != QStringLiteral("0")) {
                m_sharing = true;
                DeviceSharing::instance().attach(this, m_rhi);
            }
        }
        if (!m_compositor && m_rhi) {
            m_compositor = std::make_unique<render::Compositor>(
                m_rhi, render::CompositorOptions{render::TexturePool::Preview, m_previewDivisor, 48});
        }
    }

    void synchronize(QQuickRhiItem* item) override {
        // The GUI thread is blocked: item state may be read and written here.
        auto* monitor = static_cast<ProgramMonitor*>(item);
        m_surround = monitor->m_surroundColor;
        m_shareDevice = monitor->m_shareDecoderDevice;
        m_transparent = monitor->m_transparencyGrid;
        if (monitor->m_previewDivisor != m_previewDivisor) {
            m_previewDivisor = monitor->m_previewDivisor;
            if (m_compositor) {
                m_compositor->setPreviewDivisor(m_previewDivisor);
            }
        }
        if (monitor->m_mailbox != m_mailbox) {
            m_mailbox = monitor->m_mailbox;
            m_selection = {};
        }
        m_wake = monitor->m_wake;

        // Display-latency compensation (render()): this refresh becomes visible about one refresh interval from now.
        double refreshHz = 60.0;
        if (QQuickWindow* w = monitor->window(); w && w->screen() && w->screen()->refreshRate() > 1.0) {
            refreshHz = w->screen()->refreshRate();
        }
        m_latencyNs = static_cast<std::int64_t>(std::llround(1e9 / refreshHz));
        if (monitor->m_renderBackend.isEmpty() && m_rhi) {
            monitor->m_renderBackend = backendLabel(m_rhi);
            QMetaObject::invokeMethod(monitor, &ProgramMonitor::renderBackendChanged, Qt::QueuedConnection);
            if (monitor->m_controller) {
                monitor->m_controller->setRenderBackend(monitor->m_renderBackend.toStdString());
            }
        }
        m_scopesSource = monitor->m_scopes;
        // A renderer made again (another window, a device reset) keeps the item's picture state.
        m_hasFrame = m_hasFrame || monitor->m_hasFrame;
    }

    void render(QRhiCommandBuffer* cb) override {
        if (!m_compositor) {
            return;
        }
        // The composite that is due is taken here, on every refresh: while playing (or with composites pending) this
        // renderer asks for the next render() itself (update() below), and synchronize() only runs when the item is
        // updated from the GUI thread, which playback does not do.
        const std::uint64_t publishedBefore = m_wake ? m_wake->published.load() : 0;
        if (m_mailbox) {
            m_selection = m_mailbox->acquire(steadyNowNs() + m_latencyNs);
            if (m_selection.composite) {
                m_lastCanvas = m_selection.composite->canvas;
            }
        } else {
            m_selection = {};
        }
        if ((m_selection.composite || m_selection.black) && !m_hasFrame) {
            m_hasFrame = true;
            if (m_wake) {
                std::shared_ptr<ProgramMonitor::WakeState> wake = m_wake;
                QMetaObject::invokeMethod(
                    QCoreApplication::instance(),
                    [wake] {
                        if (ProgramMonitor* monitor = wake->monitor; monitor && !monitor->m_hasFrame) {
                            monitor->m_hasFrame = true;
                            emit monitor->hasFrameChanged();
                        }
                    },
                    Qt::QueuedConnection);
            }
        }
        render::CompositeOutput out;
        out.target = renderTarget();
        out.role = render::OutputRole::Display;
        if (!m_hasFrame) {
            // Nothing presented yet: stay transparent so the placeholder underneath shows.
            out.surround = {0.0f, 0.0f, 0.0f, 0.0f};
            out.drawCanvas = false;
            m_black.canvas = m_lastCanvas;
            (void)m_compositor->render(cb, out, m_black);
        } else {
            out.surround = {static_cast<float>(m_surround.redF()), static_cast<float>(m_surround.greenF()),
                            static_cast<float>(m_surround.blueF()), 1.0f};
            // Transparency Grid (P.2): the canvas keeps its alpha; the checkerboard under the item shows through.
            out.alpha = m_transparent ? render::OutputAlpha::Premultiplied : render::OutputAlpha::Opaque;
            if (m_selection.composite) {
                const auto stats = m_compositor->render(cb, out, m_selection.composite);
                if (stats) {
                    recordScopes(cb, stats->canvasPixels, m_selection.composite->color.displayTransform,
                                 m_selection.composite->color.management);
                }
            } else {
                // A jump seek with nothing decoded yet: the canvas is black.
                m_black.canvas = m_lastCanvas;
                (void)m_compositor->render(cb, out, m_black);
            }
        }
        // Playing or composites pending: refresh again next frame. Otherwise the publish callback wakes the item; a
        // composite published while this refresh ran saw `refreshing` still set and did not, so take it next frame.
        // (Store then load here, add then load there: sequentially consistent, so one side always sees the other.)
        if (m_wake) {
            m_wake->refreshing.store(m_selection.keepRefreshing);
        }
        if (m_selection.keepRefreshing || (m_wake && m_wake->published.load() != publishedBefore)) {
            update();
        }
    }

private:
    /// The Scopes panel's GPU scopes of the frame just drawn, at most every 30 ms; frames keep coming while a result is
    /// pending or skipped by the throttle, so a paused picture gets its scopes too.
    void recordScopes(QRhiCommandBuffer* cb, QSize canvasPixels, render::OutputTransform transform,
                      const ColorManagement& management) {
        ScopeHub& hub = ScopeHub::shared();
        if (!m_scopesSource || !hub.wanted() || canvasPixels.isEmpty()) {
            return;
        }
        if (!m_scopes) {
            m_scopes = std::make_unique<render::GpuScopes>(m_rhi);
        }
        const auto now = std::chrono::steady_clock::now();
        if (m_scopes->pending() || now - m_lastScopes < std::chrono::milliseconds(30)) {
            update(); // finish the readback / try again next frame
            return;
        }
        const auto recorded = m_scopes->record(cb, m_compositor->canvasTexture(), canvasPixels, transform, hub.settings(),
                                               [](color::ScopeData data) { ScopeHub::shared().deliver(std::move(data)); },
                                               management.isHdr() // I.9: the nits (PQ) or % (HLG) scale of an HDR project
                                                   ? std::optional<render::ScopeHdr>(render::ScopeHdr{
                                                         management.mode == ColorManagementMode::HdrHlg,
                                                         management.referenceWhiteNits, management.peakNits})
                                                   : std::nullopt);
        if (recorded) {
            m_lastScopes = now;
            update(); // the readback completes at the next frame
        }
    }

    QRhi* m_rhi = nullptr;
    bool m_sharing = false; // registered with DeviceSharing
    std::unique_ptr<render::GpuScopes> m_scopes;
    bool m_scopesSource = false;
    std::chrono::steady_clock::time_point m_lastScopes{};
    std::unique_ptr<render::Compositor> m_compositor;
    std::shared_ptr<render::CompositeMailbox> m_mailbox;
    std::shared_ptr<ProgramMonitor::WakeState> m_wake;
    render::CompositeMailbox::Selection m_selection;
    render::PreparedComposite m_black; // no layers: black canvas
    render::CanvasSize m_lastCanvas{1920, 1080};
    QColor m_surround{Qt::black};
    int m_previewDivisor = 1;
    std::int64_t m_latencyNs = 16'666'667;
    bool m_hasFrame = false; // render thread: a composite (or a seek's black) has been drawn
    bool m_shareDevice = true;
    bool m_transparent = false;
};

// ---- Item (GUI thread) ----

ProgramMonitor::ProgramMonitor(QQuickItem* parent)
    : QQuickRhiItem(parent), m_wake(std::make_shared<WakeState>()) {
    m_wake->monitor = this;
    // Transparent until the first frame (the placeholder shows through); the output is premultiplied.
    setAlphaBlending(true);
}

ProgramMonitor::~ProgramMonitor() {
    if (m_mailbox) {
        m_mailbox->setPublishCallback({});
    }
}

void ProgramMonitor::setSurroundColor(const QColor& color) {
    if (color == m_surroundColor) {
        return;
    }
    m_surroundColor = color;
    emit surroundColorChanged();
    update();
}

void ProgramMonitor::setScopes(bool scopes) {
    if (scopes == m_scopes) {
        return;
    }
    m_scopes = scopes;
    if (scopes) {
        ScopeHub::shared().addMonitor(this);
    }
    emit scopesChanged();
    update();
}

void ProgramMonitor::setShareDecoderDevice(bool share) {
    if (share == m_shareDecoderDevice) {
        return;
    }
    m_shareDecoderDevice = share;
    emit shareDecoderDeviceChanged();
}

void ProgramMonitor::setTransparencyGrid(bool on) {
    if (on == m_transparencyGrid) {
        return;
    }
    m_transparencyGrid = on;
    emit transparencyGridChanged();
    update();
}

void ProgramMonitor::setPreviewDivisor(int divisor) {
    divisor = divisor <= 1 ? 1 : divisor <= 2 ? 2 : divisor <= 4 ? 4 : 8;
    if (divisor == m_previewDivisor) {
        return;
    }
    m_previewDivisor = divisor;
    emit previewDivisorChanged();
    update();
}

void ProgramMonitor::setMailbox(std::shared_ptr<render::CompositeMailbox> mailbox) {
    if (mailbox == m_mailbox) {
        return;
    }
    if (m_mailbox) {
        m_mailbox->setPublishCallback({});
    }
    m_mailbox = std::move(mailbox);
    if (m_mailbox) {
        // Wake an idle monitor when the scheduler publishes (paused, after a seek). At most one queued call
        // is outstanding, and none while the renderer refreshes every frame anyway.
        std::shared_ptr<WakeState> wake = m_wake;
        m_mailbox->setPublishCallback([wake] {
            wake->published.fetch_add(1);
            if (wake->refreshing.load() || wake->wakePending.exchange(true)) {
                return;
            }
            QMetaObject::invokeMethod(
                QCoreApplication::instance(),
                [wake] {
                    wake->wakePending.store(false);
                    if (wake->monitor) {
                        wake->monitor->update();
                    }
                },
                Qt::QueuedConnection);
        });
    }
    update();
}

void ProgramMonitor::setPlaybackController(playback::PlaybackController* controller) {
    m_controller = controller;
    setMailbox(controller ? controller->mailbox() : nullptr);
    if (controller && !m_renderBackend.isEmpty()) {
        controller->setRenderBackend(m_renderBackend.toStdString());
    }
}

QQuickRhiItemRenderer* ProgramMonitor::createRenderer() {
    return new ProgramMonitorRenderer;
}

} // namespace wabcut::app
