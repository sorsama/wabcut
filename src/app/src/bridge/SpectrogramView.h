#pragma once

// SpectrogramView (0.7, card 047): the spectrogram of Audio Tools' Spectral tab (SpectralRepairBridge). The picture is made OFF the
// GUI thread: a worker reads the tiles of the bridge's analysis (SpectrogramCache: STFT levels, computed on demand and kept
// within a memory budget), pools them to the view's zoom, maps them to the frequency axis (logarithmic or linear) and the
// colour map, and hands the finished image back; until it arrives the last image is drawn where it belongs, so scrolling
// and zooming never wait for the analysis. The item itself only draws that image and, over it, the clip's edits, the draft
// selection of the tool in use and the playhead.
//
//   SpectrogramView { anchors.fill: parent; dark: Theme.dark; selectionShape: "rectangle"; selectionPoints: [...] }
//
// Time is seconds of the clip's source, frequency Hz, like the edits. timeAt() / xAt() / hzAt() / yAt() convert between them
// and the item's pixels (QML turns mouse positions into selections with them).

#include <wabcut/audio/dsp/Spectrogram.h>
#include <wabcut/model/SpectralRepair.h>

#include <QColor>
#include <QImage>
#include <QPointer>
#include <QQuickPaintedItem>
#include <QTimer>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <memory>
#include <stop_token>

namespace wabcut::app {

class SpectralRepairBridge;

class SpectrogramView : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT

    /// The colour map for a dark or a light surface (quiet is the surface's own tone, loud the strong one).
    Q_PROPERTY(bool dark READ dark WRITE setDark NOTIFY styleChanged FINAL)
    Q_PROPERTY(QColor selectionColor READ selectionColor WRITE setSelectionColor NOTIFY styleChanged FINAL)
    Q_PROPERTY(QColor editColor READ editColor WRITE setEditColor NOTIFY styleChanged FINAL)
    Q_PROPERTY(QColor playheadColor READ playheadColor WRITE setPlayheadColor NOTIFY styleChanged FINAL)
    Q_PROPERTY(QColor backgroundColor READ backgroundColor WRITE setBackgroundColor NOTIFY styleChanged FINAL)
    /// The tool's draft: "" (none), "rectangle" (two opposite corners), "lasso", "brush" or "tone"; its points
    /// [{time, hz}] and the brush's half-size.
    Q_PROPERTY(QString selectionShape READ selectionShape WRITE setSelectionShape NOTIFY selectionChanged FINAL)
    Q_PROPERTY(QVariantList selectionPoints READ selectionPoints WRITE setSelectionPoints NOTIFY selectionChanged FINAL)
    Q_PROPERTY(double brushRadiusSeconds READ brushRadiusSeconds WRITE setBrushRadiusSeconds NOTIFY selectionChanged FINAL)
    Q_PROPERTY(double brushRadiusOctaves READ brushRadiusOctaves WRITE setBrushRadiusOctaves NOTIFY selectionChanged FINAL)
    /// The frequency axis: its ends in Hz (the lower one is 20 Hz on the logarithmic axis, 0 on the linear one).
    Q_PROPERTY(double minHz READ minHz NOTIFY axisChanged FINAL)
    Q_PROPERTY(double maxHz READ maxHz NOTIFY axisChanged FINAL)
    Q_PROPERTY(bool logFrequency READ logFrequency NOTIFY axisChanged FINAL)
    /// The picture shows the whole analysis for the current view; progress counts the tiles being computed.
    Q_PROPERTY(bool ready READ ready NOTIFY progressChanged FINAL)
    Q_PROPERTY(bool analysing READ analysing NOTIFY progressChanged FINAL)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged FINAL)
    /// There is a picture to draw (the first image has arrived).
    Q_PROPERTY(bool hasImage READ hasImage NOTIFY progressChanged FINAL)

public:
    explicit SpectrogramView(QQuickItem* parent = nullptr);
    ~SpectrogramView() override;

    bool dark() const { return m_dark; }
    void setDark(bool dark);
    QColor selectionColor() const { return m_selectionColor; }
    void setSelectionColor(const QColor& c);
    QColor editColor() const { return m_editColor; }
    void setEditColor(const QColor& c);
    QColor playheadColor() const { return m_playheadColor; }
    void setPlayheadColor(const QColor& c);
    QColor backgroundColor() const { return m_background; }
    void setBackgroundColor(const QColor& c);
    QString selectionShape() const { return m_shape; }
    void setSelectionShape(const QString& shape);
    QVariantList selectionPoints() const { return m_points; }
    void setSelectionPoints(const QVariantList& points);
    double brushRadiusSeconds() const { return m_radiusSeconds; }
    void setBrushRadiusSeconds(double seconds);
    double brushRadiusOctaves() const { return m_radiusOctaves; }
    void setBrushRadiusOctaves(double octaves);
    double minHz() const;
    double maxHz() const;
    bool logFrequency() const;
    bool ready() const { return m_ready; }
    bool analysing() const { return m_analysing; }
    double progress() const { return m_progress; }
    bool hasImage() const { return !m_image.isNull(); }

    Q_INVOKABLE double timeAt(double x) const;
    Q_INVOKABLE double xAt(double seconds) const;
    Q_INVOKABLE double hzAt(double y) const;
    Q_INVOKABLE double yAt(double hz) const;
    /// The level (dB) the analysis holds at this pixel, or -1000 where it is not analysed yet.
    Q_INVOKABLE double levelAt(double x, double y) const;
    /// Draws the picture again now (tests, after a style change the bridge does not know about).
    Q_INVOKABLE void refresh();

    void paint(QPainter* painter) override;

signals:
    void styleChanged();
    void selectionChanged();
    void axisChanged();
    void progressChanged();

protected:
    void componentComplete() override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;

private:
    struct Request;
    static void render(Request rq, std::stop_token stop, quint64 generation, QPointer<SpectrogramView> self);
    void request();
    void startRender();
    void applyImage(const QImage& image, quint64 generation, double start, double span, int done, int total);
    void drawEdit(QPainter& p, const SpectralEdit& edit, bool draft) const;
    SpectralEdit draftEdit() const;

    QPointer<SpectralRepairBridge> m_bridge;
    bool m_dark = true;
    QColor m_selectionColor{QStringLiteral("#8B5CF6")};
    QColor m_editColor{QStringLiteral("#8B5CF6")};
    QColor m_playheadColor{Qt::white};
    QColor m_background{QStringLiteral("#0B0B0C")};
    QString m_shape;
    QVariantList m_points;
    double m_radiusSeconds = 0.05;
    double m_radiusOctaves = 0.25;

    QImage m_image;
    double m_imageStart = 0.0;
    double m_imageSpan = 1.0;
    bool m_ready = false;
    bool m_analysing = false;
    double m_progress = 0.0;
    quint64 m_generation = 0;
    std::stop_source m_stop;
    QTimer m_debounce;
    std::shared_ptr<audio::dsp::SpectrogramCache> m_cache;
};

} // namespace wabcut::app
