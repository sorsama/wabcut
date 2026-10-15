#pragma once

// StillFrame: shows one QImage, fitted and centred (M5i: the Trim view's outgoing and incoming frames). A scene-graph
// image node; the texture is uploaded when the image changes.

#include <QImage>
#include <QQuickItem>
#include <QtQml/qqmlregistration.h>

namespace wabcut::app {

class StillFrame : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QImage image READ image WRITE setImage NOTIFY imageChanged FINAL)
    /// The image is not null.
    Q_PROPERTY(bool hasImage READ hasImage NOTIFY imageChanged FINAL)

public:
    explicit StillFrame(QQuickItem* parent = nullptr);
    QImage image() const { return m_image; }
    void setImage(const QImage& image);
    bool hasImage() const { return !m_image.isNull(); }

signals:
    void imageChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) override;

private:
    QImage m_image;
    bool m_dirty = false;
};

} // namespace wabcut::app
