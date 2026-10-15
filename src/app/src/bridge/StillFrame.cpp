#include "StillFrame.h"

#include <QQuickWindow>
#include <QSGImageNode>

namespace wabcut::app {

StillFrame::StillFrame(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents);
}

void StillFrame::setImage(const QImage& image) {
    if (image.isNull() && m_image.isNull())
        return;
    m_image = image;
    m_dirty = true;
    emit imageChanged();
    update();
}

QSGNode* StillFrame::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) {
    auto* node = static_cast<QSGImageNode*>(oldNode);
    if (m_image.isNull() || width() <= 0 || height() <= 0 || !window()) {
        delete node;
        return nullptr;
    }
    if (!node) {
        node = window()->createImageNode();
        node->setOwnsTexture(true);
        m_dirty = true;
    }
    if (m_dirty) {
        node->setTexture(window()->createTextureFromImage(m_image));
        m_dirty = false;
    }
    const QSizeF fitted = QSizeF(m_image.size()).scaled(size(), Qt::KeepAspectRatio);
    node->setRect(QRectF((width() - fitted.width()) / 2, (height() - fitted.height()) / 2, fitted.width(), fitted.height()));
    node->setFiltering(QSGTexture::Linear);
    return node;
}

} // namespace wabcut::app
