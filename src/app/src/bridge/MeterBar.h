#pragma once

// MeterBar: a vertical peak/RMS level meter for the Audio Track Mixer (M5g), drawn in the scene graph (a few
// rectangles per channel), cheap enough for every strip at ~30 Hz:
//   * the RMS level fills from the bottom in three zones (lowColor below -18 dBFS, midColor to -6, highColor above);
//   * the peak is a thin line, the peak hold a short bar in its zone's colour;
//   * `clipped` latches when a channel reached 0 dBFS (AudioMixer.clearClip resets it).
// It reads AudioMixerBridge::meter(channel) in C++ when the bridge announces new levels (metersUpdated) and repaints
// only when they changed; QML sees only `clipped` and `peakText`, which change far less often.

#include "AudioMixerBridge.h"

#include <QColor>
#include <QPointer>
#include <QQuickItem>
#include <QString>
#include <QtQml/qqmlregistration.h>

namespace wabcut::app {

class MeterBar : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(wabcut::app::AudioMixerBridge* source READ source WRITE setSource NOTIFY sourceChanged FINAL)
    /// "t12" (a track) or "master".
    Q_PROPERTY(QString channel READ channel WRITE setChannel NOTIFY channelChanged FINAL)
    /// The bottom of the scale (dBFS).
    Q_PROPERTY(double floorDb READ floorDb WRITE setFloorDb NOTIFY appearanceChanged FINAL)
    Q_PROPERTY(QColor trackColor READ trackColor WRITE setTrackColor NOTIFY appearanceChanged FINAL)
    Q_PROPERTY(QColor lowColor READ lowColor WRITE setLowColor NOTIFY appearanceChanged FINAL)
    Q_PROPERTY(QColor midColor READ midColor WRITE setMidColor NOTIFY appearanceChanged FINAL)
    Q_PROPERTY(QColor highColor READ highColor WRITE setHighColor NOTIFY appearanceChanged FINAL)
    Q_PROPERTY(QColor peakColor READ peakColor WRITE setPeakColor NOTIFY appearanceChanged FINAL)
    Q_PROPERTY(bool clipped READ clipped NOTIFY levelsChanged FINAL)
    /// The highest peak hold, "−6.2" / "−∞" (one decimal: changes a few times a second at most).
    Q_PROPERTY(QString peakText READ peakText NOTIFY levelsChanged FINAL)
    /// Channels shown now (0 when nothing plays).
    Q_PROPERTY(int channels READ channels NOTIFY levelsChanged FINAL)

public:
    explicit MeterBar(QQuickItem* parent = nullptr);

    AudioMixerBridge* source() const { return m_source; }
    void setSource(AudioMixerBridge* source);
    QString channel() const { return m_channel; }
    void setChannel(const QString& channel);
    double floorDb() const { return m_floorDb; }
    void setFloorDb(double db);
    QColor trackColor() const { return m_trackColor; }
    void setTrackColor(const QColor& c);
    QColor lowColor() const { return m_lowColor; }
    void setLowColor(const QColor& c);
    QColor midColor() const { return m_midColor; }
    void setMidColor(const QColor& c);
    QColor highColor() const { return m_highColor; }
    void setHighColor(const QColor& c);
    QColor peakColor() const { return m_peakColor; }
    void setPeakColor(const QColor& c);
    bool clipped() const { return m_levels.clipped; }
    QString peakText() const { return m_peakText; }
    int channels() const { return m_levels.channels; }

    /// Where `db` sits on the bar, 0 (floor) to 1 (0 dBFS).
    double fraction(double db) const;
    /// Rectangles drawn last (tests): fills, peak lines and holds, in item coordinates.
    int drawnRects() const { return m_drawnRects; }

signals:
    void sourceChanged();
    void channelChanged();
    void appearanceChanged();
    void levelsChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) override;

private:
    void readLevels();

    QPointer<AudioMixerBridge> m_source;
    QString m_channel;
    double m_floorDb = -60.0;
    QColor m_trackColor{0x14, 0x13, 0x17};
    QColor m_lowColor{0x34, 0xD3, 0x99};
    QColor m_midColor{0xFB, 0xBF, 0x24};
    QColor m_highColor{0xF4, 0x3F, 0x5E};
    QColor m_peakColor{Qt::white};
    AudioMixerBridge::MeterLevels m_levels;
    QString m_peakText;
    int m_drawnRects = 0;
};

} // namespace wabcut::app
