#pragma once

// The Scopes panel (I.5, docs/color.md "Scopes"): waveform / parade, vectorscope and histogram of the
// program as shown, computed on the GPU by the program monitor (render::GpuScopes) at about 30 Hz while the panel is
// active. Owner: Graphics.
//
//   ScopeHub      thread-safe meeting point: whether scopes are wanted (and their settings), the latest result from
//                 the monitor's render thread, the monitors to wake when scopes are first wanted (paused frames).
//   ScopesBridge  QML singleton `Scopes`: active, waveform mode, vectorscope zoom, statistics, generation.
//   ScopeView     a QQuickPaintedItem drawing one scope from the latest result, with its graticule (IRE / nits lines,
//                 75 % / 100 % targets and the skin-tone line).

#include <wabcut/color/Scopes.h>

#include <QColor>
#include <QObject>
#include <QPointer>
#include <QQuickPaintedItem>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

class QJSEngine;
class QQmlEngine;
class QQuickItem;

namespace wabcut::app {

class ScopeHub {
public:
    static ScopeHub& shared();

    bool wanted() const { return m_wanted.load(std::memory_order_acquire); }
    void setWanted(bool wanted);
    color::ScopeSettings settings() const;
    void setSettings(const color::ScopeSettings& settings);
    /// Render thread: a new result (also counts the updates per second).
    void deliver(color::ScopeData data);
    std::shared_ptr<const color::ScopeData> latest() const;
    std::uint64_t generation() const { return m_generation.load(std::memory_order_acquire); }
    /// Scope updates delivered over the last second.
    double rate() const;
    /// GUI thread: called (queued) after each delivery.
    void setListener(QObject* receiver, std::function<void()> onDelivered);
    /// Monitors register so they can be asked for a frame when scopes become wanted (a paused picture).
    void addMonitor(QQuickItem* monitor);
    void requestFrames();

private:
    std::atomic<bool> m_wanted{false};
    std::atomic<std::uint64_t> m_generation{0};
    mutable std::mutex m_mutex;
    color::ScopeSettings m_settings;
    std::shared_ptr<const color::ScopeData> m_latest;
    std::vector<std::chrono::steady_clock::time_point> m_times;
    QPointer<QObject> m_receiver;
    std::function<void()> m_onDelivered;
    std::vector<QPointer<QQuickItem>> m_monitors;
};

class ScopesBridge : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(Scopes)
    QML_SINGLETON

    /// The panel is visible: the monitor computes scopes (else it does not).
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY settingsChanged FINAL)
    /// Waveform: 0 luma, 1 RGB parade, 2 YCbCr parade.
    Q_PROPERTY(int waveformMode READ waveformMode WRITE setWaveformMode NOTIFY settingsChanged FINAL)
    /// Vectorscope zoom: 1, 2 or 5.
    Q_PROPERTY(int vectorZoom READ vectorZoom WRITE setVectorZoom NOTIFY settingsChanged FINAL)
    /// The latest statistics: exposure, cast, luma percentiles (IRE), clipping, saturation (color::ScopeStats).
    Q_PROPERTY(QVariantMap stats READ stats NOTIFY updated FINAL)
    /// Increases with every result (views repaint on it); `rate` = results per second.
    Q_PROPERTY(int generation READ generation NOTIFY updated FINAL)
    Q_PROPERTY(double rate READ rate NOTIFY updated FINAL)

public:
    explicit ScopesBridge(QObject* parent = nullptr);
    ~ScopesBridge() override;
    static ScopesBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);

    bool active() const { return m_active; }
    void setActive(bool active);
    int waveformMode() const { return m_waveformMode; }
    void setWaveformMode(int mode);
    int vectorZoom() const { return m_vectorZoom; }
    void setVectorZoom(int zoom);
    QVariantMap stats() const { return m_stats; }
    int generation() const { return static_cast<int>(ScopeHub::shared().generation()); }
    double rate() const { return ScopeHub::shared().rate(); }

signals:
    void settingsChanged();
    void updated();

private:
    void onDelivered();

    bool m_active = false;
    int m_waveformMode = 0;
    int m_vectorZoom = 1;
    QVariantMap m_stats;
};

class ScopeView : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT

    /// "waveform", "parade", "ycbcr", "vectorscope" or "histogram".
    Q_PROPERTY(QString kind READ kind WRITE setKind NOTIFY kindChanged FINAL)
    /// Set to Scopes.generation: a change repaints.
    Q_PROPERTY(int generation READ generation WRITE setGeneration NOTIFY generationChanged FINAL)
    /// Theme colours (the panel binds them): background, graticule lines, labels, the skin-tone line.
    Q_PROPERTY(QColor background READ background WRITE setBackground NOTIFY colorsChanged FINAL)
    Q_PROPERTY(QColor gridColor READ gridColor WRITE setGridColor NOTIFY colorsChanged FINAL)
    Q_PROPERTY(QColor labelColor READ labelColor WRITE setLabelColor NOTIFY colorsChanged FINAL)
    Q_PROPERTY(QColor skinColor READ skinColor WRITE setSkinColor NOTIFY colorsChanged FINAL)

public:
    explicit ScopeView(QQuickItem* parent = nullptr);
    QString kind() const { return m_kind; }
    void setKind(const QString& kind);
    int generation() const { return m_generation; }
    void setGeneration(int generation);
    QColor background() const { return m_background; }
    void setBackground(const QColor& c);
    QColor gridColor() const { return m_grid; }
    void setGridColor(const QColor& c);
    QColor labelColor() const { return m_label; }
    void setLabelColor(const QColor& c);
    QColor skinColor() const { return m_skin; }
    void setSkinColor(const QColor& c);
    void paint(QPainter* painter) override;

signals:
    void kindChanged();
    void generationChanged();
    void colorsChanged();

private:
    QString m_kind = QStringLiteral("waveform");
    int m_generation = 0;
    QColor m_background{Qt::black};
    QColor m_grid{Qt::gray};
    QColor m_label{Qt::white};
    QColor m_skin{Qt::yellow};
};

} // namespace wabcut::app
