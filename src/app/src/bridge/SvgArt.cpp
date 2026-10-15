#include "SvgArt.h"

#include <QFile>
#include <QPainter>
#include <QSvgRenderer>

namespace wabcut::app {

SvgArt::SvgArt(QQuickItem* parent) : QQuickPaintedItem(parent) {
    setAntialiasing(true);
}

SvgArt::~SvgArt() = default;

void SvgArt::setSource(const QString& source) {
    if (source == m_source)
        return;
    m_source = source;
    QFile file(QStringLiteral(":/qt/qml/Wabcut/resources/") + source);
    m_svg = file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    reload();
    const QSizeF size = artSize();
    setImplicitSize(size.width(), size.height());
    emit sourceChanged();
}

void SvgArt::setTint(const QColor& tint) {
    if (tint == m_tint)
        return;
    m_tint = tint;
    reload();
    emit tintChanged();
}

void SvgArt::reload() {
    QByteArray svg = m_svg;
    svg.replace("currentColor", m_tint.name(QColor::HexRgb).toLatin1());
    if (m_tint.alphaF() < 1.0f) {
        // The tint's alpha as the line opacity (SVG colours carry none).
        svg.replace("stroke=\"" + m_tint.name(QColor::HexRgb).toLatin1() + "\"",
                    "stroke=\"" + m_tint.name(QColor::HexRgb).toLatin1() + "\" stroke-opacity=\"" +
                        QByteArray::number(m_tint.alphaF(), 'f', 3) + "\"");
    }
    m_renderer = svg.isEmpty() ? nullptr : std::make_unique<QSvgRenderer>(svg);
    update();
}

QSizeF SvgArt::artSize() const {
    return m_renderer && m_renderer->isValid() ? m_renderer->viewBoxF().size() : QSizeF();
}

bool SvgArt::valid() const { return m_renderer && m_renderer->isValid(); }

void SvgArt::paint(QPainter* painter) {
    if (!valid() || width() <= 0 || height() <= 0)
        return;
    QSizeF art = artSize();
    art.scale(size(), Qt::KeepAspectRatio);
    const QRectF target((width() - art.width()) / 2, (height() - art.height()) / 2, art.width(), art.height());
    m_renderer->render(painter, target);
}

} // namespace wabcut::app
