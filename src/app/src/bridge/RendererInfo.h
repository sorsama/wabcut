#pragma once

// RendererInfo: reports the graphics API and adapter a QQuickWindow actually renders with, for the GPU status
// pill (architecture principle 4: "the UI always shows the actual backends").
//
//   RendererInfo { id: renderer; window: root.Window.window }
//   StatusPill { detail: renderer.ready ? renderer.summary : qsTr("detecting…") }
//
// The values come from QQuickWindow::rhi() / QRhi::driverInfo() when the window's scene graph is initialized
// (and, as a fallback, at its first rendered frame). Those signals arrive on the render thread, the only place the
// window's QRhi may be touched: a direct connection reads the two strings there and queues only the copies to this
// GUI-thread object. For an offscreen grab they are emitted on the GUI thread and applied at once, while the
// temporary QRhi still exists. Teardown: each window attachment has a shared link that this object severs before
// it detaches or dies, so a capture racing with it posts nothing. The latest capture is also kept process-wide,
// so a new RendererInfo starts with the known values.

#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QQuickWindow>
#include <QString>
#include <QtQml/qqmlregistration.h>

#include <memory>
#include <vector>

namespace wabcut::app {

class RendererInfo : public QObject {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QQuickWindow* window READ window WRITE setWindow NOTIFY windowChanged FINAL)
    Q_PROPERTY(bool ready READ ready NOTIFY infoChanged FINAL)
    Q_PROPERTY(QString api READ api NOTIFY infoChanged FINAL)
    Q_PROPERTY(QString adapter READ adapter NOTIFY infoChanged FINAL)
    Q_PROPERTY(QString shortAdapter READ shortAdapter NOTIFY infoChanged FINAL)
    Q_PROPERTY(QString summary READ summary NOTIFY infoChanged FINAL)

public:
    explicit RendererInfo(QObject* parent = nullptr);
    ~RendererInfo() override;

    QQuickWindow* window() const { return m_window; }
    void setWindow(QQuickWindow* window);

    bool ready() const { return !m_api.isEmpty(); }
    // "D3D11", "D3D12", "Vulkan", "OpenGL", "Metal", "Null" or "Software".
    QString api() const { return m_api; }
    // Full device name as reported by the driver, e.g. "NVIDIA GeForce RTX 3060".
    QString adapter() const { return m_adapter; }
    // Adapter without vendor/brand noise, e.g. "RTX 3060".
    QString shortAdapter() const { return shortAdapterName(m_adapter); }
    // "D3D11 · RTX 3060" (API only when the adapter is unknown).
    QString summary() const;

    // Strips vendor and trademark noise from a driver device name ("NVIDIA GeForce RTX 3060" → "RTX 3060",
    // "AMD Radeon RX 7800 XT" → "Radeon RX 7800 XT", "Intel(R) UHD Graphics 770" → "UHD Graphics 770").
    static QString shortAdapterName(const QString& deviceName);

signals:
    void windowChanged();
    void infoChanged();

private:
    struct Link;

    void disconnectWindow();
    void apply(const QString& api, const QString& adapter); // GUI thread

    QPointer<QQuickWindow> m_window;
    std::shared_ptr<Link> m_link;
    std::vector<QMetaObject::Connection> m_connections;
    QString m_api;
    QString m_adapter;
};

} // namespace wabcut::app
