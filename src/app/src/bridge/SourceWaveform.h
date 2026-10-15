#pragma once

// SourceWaveform: the Source Monitor's picture for audio-only media (M5h): the loaded source's whole length as a
// waveform, one lane per audio stream (at most two), from ProjectController's peak source (real peaks once they are
// decoded, the placeholder envelope until then). While a long recording's peaks are still decoding, the part decoded
// so far is drawn and the rest of the lane is shaded in baseColor (MediaPeakSource::buildingUntil). It repaints when
// the source or the peaks change (partial results too). Interaction (click to move, Ctrl/Cmd-drag to scrub) is QML's;
// this item only draws.
//
//   SourceWaveform { anchors.fill: parent; color: Theme.accentStroke; baseColor: Theme.guide }

#include <QColor>
#include <QPointer>
#include <QQuickPaintedItem>
#include <QtQml/qqmlregistration.h>

namespace wabcut::app {

class ProjectController;
class SourceMonitorBridge;

class SourceWaveform : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorChanged FINAL)
    /// The zero line and the lane divider.
    Q_PROPERTY(QColor baseColor READ baseColor WRITE setBaseColor NOTIFY colorChanged FINAL)
    /// Lanes drawn (the media's audio streams, at most two).
    Q_PROPERTY(int lanes READ lanes NOTIFY lanesChanged FINAL)

public:
    explicit SourceWaveform(QQuickItem* parent = nullptr);

    QColor color() const { return m_color; }
    void setColor(const QColor& color);
    QColor baseColor() const { return m_baseColor; }
    void setBaseColor(const QColor& color);
    int lanes() const { return m_lanes; }

    void paint(QPainter* painter) override;

signals:
    void colorChanged();
    void lanesChanged();

protected:
    void componentComplete() override;

private:
    QPointer<SourceMonitorBridge> m_bridge;
    QPointer<ProjectController> m_controller;
    QColor m_color{Qt::white};
    QColor m_baseColor{Qt::gray};
    int m_lanes = 0;
};

} // namespace wabcut::app
