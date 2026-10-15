#include "SourceWaveform.h"

#include "MediaBridge.h"
#include "ProjectController.h"
#include "SourceMonitorBridge.h"

#include <QPainter>
#include <QPainterPath>
#include <QQmlEngine>

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace wabcut::app {

SourceWaveform::SourceWaveform(QQuickItem* parent) : QQuickPaintedItem(parent) {
    setAntialiasing(true);
}

void SourceWaveform::setColor(const QColor& color) {
    if (color == m_color)
        return;
    m_color = color;
    emit colorChanged();
    update();
}

void SourceWaveform::setBaseColor(const QColor& color) {
    if (color == m_baseColor)
        return;
    m_baseColor = color;
    emit colorChanged();
    update();
}

void SourceWaveform::componentComplete() {
    QQuickPaintedItem::componentComplete();
    QQmlEngine* engine = qmlEngine(this);
    if (!engine)
        return;
    m_bridge = engine->singletonInstance<SourceMonitorBridge*>("Wabcut", "SourceMonitor");
    m_controller = engine->singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    const auto refresh = [this] { update(); };
    if (m_bridge)
        connect(m_bridge, &SourceMonitorBridge::sourceChanged, this, refresh);
    if (m_controller)
        connect(m_controller, &ProjectController::peaksChanged, this, refresh);
    connect(this, &QQuickItem::widthChanged, this, refresh);
    connect(this, &QQuickItem::heightChanged, this, refresh);
}

void SourceWaveform::paint(QPainter* painter) {
    int lanes = 0;
    const MediaItem* item = nullptr;
    ProjectSnapshot snap;
    if (m_bridge && m_controller && m_bridge->loaded() && m_bridge->lengthTicks()) {
        snap = m_controller->snapshot();
        item = snap->findMedia(m_bridge->mediaId());
        if (item)
            lanes = static_cast<int>(std::min<std::size_t>(2, item->info.audio.size()));
    }
    if (lanes != m_lanes) {
        m_lanes = lanes;
        emit lanesChanged();
    }
    if (lanes == 0 || width() < 2 || height() < 2)
        return;

    const int columns = std::max(1, static_cast<int>(std::floor(width())));
    const double laneHeight = height() / lanes;
    const std::shared_ptr<const PeakSource> peaks = m_controller->peakSource();
    const MediaPeakSource* building = m_controller->mediaPeakSource();
    const Ticks length = *m_bridge->lengthTicks();
    std::vector<float> values(static_cast<std::size_t>(columns));
    for (int lane = 0; lane < lanes; ++lane) {
        const double mid = laneHeight * (lane + 0.5);
        const double reach = laneHeight * 0.45;
        painter->setPen(QPen(m_baseColor, 1));
        painter->drawLine(QPointF(0, mid), QPointF(width(), mid));
        if (lane > 0)
            painter->drawLine(QPointF(0, laneHeight * lane), QPointF(width(), laneHeight * lane));
        const StreamRef stream{StreamKind::Audio, lane};
        const bool have = peaks && peaks->peaks(item->id, stream, Ticks{}, length, values);
        // A long recording's peaks still decoding: draw the part decoded so far and shade the rest (it is not
        // silence), redrawn as partial results arrive (peaksChanged).
        int drawn = columns;
        if (building && length.value > 0) {
            if (const std::optional<Ticks> until = building->buildingUntil(item->id, stream)) {
                const double share = std::clamp(until->toSeconds() / length.toSeconds(), 0.0, 1.0);
                drawn = std::clamp(static_cast<int>(std::floor(share * columns)), 0, columns);
                QColor shade = m_baseColor;
                shade.setAlphaF(0.25f);
                painter->fillRect(QRectF(drawn, mid - reach, width() - drawn, 2.0 * reach), shade);
            }
        }
        if (!have)
            continue; // not decoded yet: the zero line until peaksChanged
        QPainterPath path;
        for (int x = 0; x < drawn; ++x) {
            const double h = std::clamp<double>(values[static_cast<std::size_t>(x)], 0.0, 1.0) * reach;
            path.addRect(QRectF(x, mid - h, 1.0, std::max(1.0, 2.0 * h)));
        }
        painter->fillPath(path, m_color);
    }
}

} // namespace wabcut::app
