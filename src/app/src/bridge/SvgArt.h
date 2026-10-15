#pragma once

// SvgArt: a bundled SVG drawn at the item's size (aspect fit), with `currentColor` replaced by `tint` before
// rendering, so line illustrations follow the theme while their UV-beam accents keep their own colours (Icon.qml
// recolours a whole image, which would flatten the beam). Used for the empty-state illustrations and the logo.
//
//   SvgArt { source: "illustrations/empty-library.svg"; tint: Theme.textTertiary }
//
// `source` is relative to the module's resources (qrc:/qt/qml/Wabcut/resources/). Rendered with QSvgRenderer into a
// cached texture (QQuickPaintedItem): repainted only when the source, the tint or the size change.

#include <QColor>
#include <QQuickPaintedItem>
#include <QString>
#include <QtQml/qqmlregistration.h>

#include <memory>

class QSvgRenderer;

namespace wabcut::app {

class SvgArt : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QString source READ source WRITE setSource NOTIFY sourceChanged FINAL)
    Q_PROPERTY(QColor tint READ tint WRITE setTint NOTIFY tintChanged FINAL)
    /// The SVG's own size (its viewBox), for implicit sizing.
    Q_PROPERTY(QSizeF artSize READ artSize NOTIFY sourceChanged FINAL)
    Q_PROPERTY(bool valid READ valid NOTIFY sourceChanged FINAL)

public:
    explicit SvgArt(QQuickItem* parent = nullptr);
    ~SvgArt() override;

    QString source() const { return m_source; }
    void setSource(const QString& source);
    QColor tint() const { return m_tint; }
    void setTint(const QColor& tint);
    QSizeF artSize() const;
    bool valid() const;

    void paint(QPainter* painter) override;

signals:
    void sourceChanged();
    void tintChanged();

private:
    void reload();

    QString m_source;
    QColor m_tint{Qt::white};
    QByteArray m_svg;
    std::unique_ptr<QSvgRenderer> m_renderer;
};

} // namespace wabcut::app
